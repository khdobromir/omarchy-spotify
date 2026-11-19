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

## Desktop player

The separate **Omarchy Spotify** app plays songs, albums, and playlists on this computer when you select them in the library. Playback runs through [Spotify Soloist](https://developer.spotify.com/documentation/soloist). Install the official [Soloist build](https://developer.spotify.com/documentation/soloist/reference/downloads-and-updates) as `soloist` on your PATH or in `~/.local/bin`.

Build and install with Qt 6, CMake, Ninja, and Clang:

```bash
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/build
ctest --test-dir desktop/build --output-on-failure
cmake --install desktop/build --prefix "$HOME/.local"
```

Open **Omarchy Spotify** from the launcher or choose **Open desktop player** in the widget. Enter your own [Soloist API key](https://developer.spotify.com/dashboard/soloist), then select **Omarchy Spotify** in the device picker on your phone once. The app reuses the widget’s Web API authorization for the library. Closing the app stops the local player.

Choose a track in the widget to play it through the local Soloist. Choose an album or playlist to browse its tracks. The **Play** button on the collection page starts the whole collection. A click on an individual track starts that URI on its own because Soloist’s `ctl play` does not accept an offset within a collection. In Spotify Web API Development Mode, the app can list tracks in [playlists you own or collaborate on](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide#playlist-endpoint-renames).

The album tab opens in **Compact grid** on first launch and remembers your choice. Use **Ctrl+1**, **Ctrl+2**, and **Ctrl+3** for the library tabs, **Ctrl+G** for the album layout, and **Alt+Left** to leave a collection. **Enter** opens the selected collection or plays the selected track, and **Space** toggles playback. **F11** opens the full-screen player and **Esc** returns to the library.

On Omarchy’s Lua-based Hyprland, the app registers a session rule for the `omarchy-spotify` window class before it shows the window. The rule floats and centers it. The app sizes it against the focused output’s usable area. A second invocation sends an activation request to the running player and brings its window to the current workspace. No Hyprland configuration file is required.

The launcher accepts one optional Spotify track, album, or playlist URI, for example `omarchy-spotify spotify:album:4m2880jivSbbyEGAKfITCa`. The primary instance holds the instance lock and listens on a user-only local socket in the session runtime directory. Secondary invocations send a bounded request, wait for acknowledgment, and exit.

Omarchy theme and translation support use the [Omarchy app templates](desktop/TEMPLATE-LICENSE).
