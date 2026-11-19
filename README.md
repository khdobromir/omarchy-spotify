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

Open **Omarchy Spotify** from the launcher. Enter your own [Soloist API key](https://developer.spotify.com/dashboard/soloist), then select **Omarchy Spotify** in the device picker on your phone once. The app reuses the plugin’s Web API authorization for the library. Space toggles playback, and Enter plays the selected library item when the list has focus. Closing the app stops the local player.

Omarchy theme and translation support use the [Omarchy app templates](desktop/TEMPLATE-LICENSE).
