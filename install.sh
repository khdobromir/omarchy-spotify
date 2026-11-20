#!/usr/bin/env bash
set -euo pipefail

prefix="$HOME/.local"
config="${XDG_CONFIG_HOME:-$HOME/.config}"
state="${XDG_STATE_HOME:-$HOME/.local/state}/omarchy-spotify"
package="$prefix/share/omarchy-spotify"
plugin="$HOME/.config/omarchy/plugins/khdobromir.spotify"
units="$prefix/share/systemd/user"
mkdir -p "$state"

if [[ ${1:-} == --uninstall && $# == 1 ]]; then
    if [[ -L $plugin && $(readlink -- "$plugin") == "$package/widget" ]]; then
        omarchy plugin disable khdobromir.spotify
        unlink -- "$plugin"
        omarchy-shell shell rescanPlugins
    fi
    if systemctl --user show-environment >/dev/null 2>&1; then
        systemctl --user stop omarchy-spotify.socket omarchy-spotify.service
        systemctl --user disable omarchy-spotify.socket omarchy-spotify.service
    elif [[ -n ${XDG_RUNTIME_DIR:-} && -f $XDG_RUNTIME_DIR/omarchy-spotify/core.lock ]]; then
        if ! flock -n "$XDG_RUNTIME_DIR/omarchy-spotify/core.lock" true; then
            printf '%s\n' 'Stop the running standalone Core before uninstalling.' >&2
            exit 1
        fi
    fi
    if [[ -f $state/install-manifest ]]; then
        while IFS= read -r file; do
            case "$file" in
                "$prefix"/bin/omarchy-spotify|"$prefix"/bin/omarchy-spotify-core|"$prefix"/bin/omarchy-spotify-open|\
                "$prefix"/share/applications/omarchy-spotify.desktop|"$prefix"/share/licenses/omarchy-spotify/*|\
                "$units"/omarchy-spotify.socket|"$units"/omarchy-spotify.service|"$package"/*)
                    if [[ -f $file || -L $file ]]; then
                        unlink -- "$file"
                    fi ;;
            esac
        done < "$state/install-manifest"
        unlink -- "$state/install-manifest"
    fi
    if systemctl --user show-environment >/dev/null 2>&1; then
        systemctl --user daemon-reload
    fi
    exit 0
fi

if [[ $# -lt 1 || $# -gt 2 || (${2:-} != "" && ${2:-} != --widget) ]]; then
    printf '%s\n' 'Usage: ./install.sh BUILD_DIR [--widget] | ./install.sh --uninstall' >&2
    exit 2
fi

build=$1
cmake --install "$build" --prefix "$prefix"
sort -u "$build/install_manifest.txt" \
    <(if [[ -f $state/install-manifest ]]; then cat "$state/install-manifest"; fi) > "$state/install-manifest.new"
mv -f -- "$state/install-manifest.new" "$state/install-manifest"

if systemctl --user show-environment >/dev/null 2>&1; then
    systemctl --user link "$units/omarchy-spotify.service" "$units/omarchy-spotify.socket"
    systemctl --user daemon-reload
    if systemctl --user is-active --quiet omarchy-spotify.service; then
        systemctl --user restart omarchy-spotify.service
    fi
    systemctl --user enable --now omarchy-spotify.socket
fi

if [[ ${2:-} == --widget || ( -L $plugin && $(readlink -- "$plugin") == "$package/widget" ) ]]; then
    if [[ $config != "$HOME/.config" ]]; then
        printf '%s\n' 'This Omarchy version detects plugins in ~/.config/omarchy/plugins.' >&2
    fi
    omarchy plugin validate "$package/widget"
    mkdir -p "$(dirname -- "$plugin")"
    if [[ -e $plugin && ! -L $plugin ]]; then
        backup=$(mktemp -d "$state/widget-backup.XXXXXX")
        mv -- "$plugin" "$backup/plugin"
        printf 'Previous widget saved in %s\n' "$backup/plugin"
    fi
    if [[ ! -L $plugin ]]; then
        ln -s -- "$package/widget" "$plugin"
    elif [[ $(readlink -- "$plugin") != "$package/widget" ]]; then
        printf 'Unexpected widget path %s\n' "$plugin" >&2
        exit 1
    fi
    omarchy-shell shell rescanPlugins
    omarchy-shell shell reloadPlugins
    if [[ ${2:-} == --widget ]]; then
        omarchy plugin enable khdobromir.spotify
    fi
fi
