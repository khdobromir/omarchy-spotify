# Omarchy Spotify

Shows the current Spotify track in the Omarchy bar, even when it’s playing on another device. Open the widget to skip tracks, pause, resume, or seek on the active Spotify Connect device. You can add or remove the current song from Liked Songs and browse your liked tracks, saved albums, and playlists in **Library**.

## Install

Requires an Omarchy version with shell plugin support, Python 3, and a Spotify Premium account.

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard). Select **Web API**, add `http://127.0.0.1:8765/callback` under **Redirect URIs**, and copy the **Client ID** from **Settings**.
2. Save your Client ID locally and install the plugin.

   ```bash
   config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy"
   mkdir -p "$config_dir"
   printf '%s\n' 'YOUR_CLIENT_ID' > "$config_dir/spotify-client-id"
   omarchy plugin add https://github.com/khdobromir/omarchy-spotify.git --enable
   ```

3. Left-click the Spotify icon in the bar, choose **Connect Spotify**, and approve playback and library access in your browser. If you already connected the read-only widget, choose **Enable controls and library** to grant the new permissions.

The plugin checks Spotify every 10 seconds while a song is playing and every 25 seconds while paused or idle. It uses OAuth with PKCE and keeps tokens in `${XDG_STATE_HOME:-~/.local/state}/omarchy/spotify/`. **Disconnect** stops updates and removes the local tokens.
