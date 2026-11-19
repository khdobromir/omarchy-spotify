import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

import spotify


CLIENT_ID = "a" * 32


class SpotifyTests(unittest.TestCase):
    def test_upgrade_preserves_playback_and_requires_consent_for_new_actions(self):
        tokens = {"client_id": CLIENT_ID, "access_token": "old", "expires_at": 2**40}
        with patch.object(spotify, "load_tokens", return_value=tokens), patch.object(
            spotify, "request_json", return_value={}
        ) as request:
            self.assertEqual(spotify.account_operation(CLIENT_ID, "status", ()),
                             {"status": "idle", "extended": False})
            self.assertEqual(spotify.account_operation(CLIENT_ID, "next", ("test-device",)),
                             {"status": "reauthorize"})
            request.assert_called_once()

    def test_playback_commands_and_stale_seek(self):
        with patch.object(spotify, "request_json", return_value={}) as request:
            for command, method in (("next", "POST"), ("previous", "POST"),
                                    ("pause", "PUT"), ("play", "PUT")):
                with self.subTest(command=command):
                    self.assertEqual(spotify.perform(command, ("device+1",), "test"),
                                     {"status": "ok"})
                    request.assert_called_with(spotify.PLAYER_URL + "/" + command + "?device_id=device%2B1",
                                               method=method, token="test")
            request.reset_mock()
            self.assertEqual(spotify.perform("seek", ("device", "a" * 22, "42000"), "test"),
                             {"status": "target_changed"})
            request.assert_called_once_with(spotify.PLAYER_URL, token="test")
            request.return_value = {
                "device": {"id": "device", "is_active": True},
                "item": {"id": "a" * 22, "type": "track", "name": "Song", "duration_ms": 42000},
            }
            self.assertEqual(spotify.perform("seek", ("device", "a" * 22, "43000"), "test"),
                             {"status": "ok"})
            request.assert_called_with(spotify.PLAYER_URL + "/seek?device_id=device&position_ms=41999",
                                       method="PUT", token="test")

    def test_favorite_uses_library_uri_and_explicit_mutation(self):
        track_id = "a" * 22
        query = "?uris=spotify%3Atrack%3A" + track_id
        with patch.object(spotify, "request_json", return_value=[True]) as request:
            self.assertEqual(spotify.perform("favorite", (track_id,), "test"),
                             {"status": "ok", "saved": True})
            request.assert_called_with(spotify.API_URL + "/me/library/contains" + query, token="test")
            for action, method, saved in (("save", "PUT", True), ("remove", "DELETE", False)):
                self.assertEqual(spotify.perform(action, (track_id,), "test"),
                                 {"status": "ok", "saved": saved})
                request.assert_called_with(spotify.API_URL + "/me/library" + query,
                                           method=method, token="test")

    def test_library_pages_map_items_and_never_follow_response_url(self):
        for kind, singular, entry in (
            ("tracks", "track", {"track": {"id": "a" * 22, "name": "<Track>"}}),
            ("albums", "album", {"album": {"id": "a" * 22, "name": "<Album>"}}),
            ("playlists", "playlist", {"id": "a" * 22, "name": "<Playlist>"}),
        ):
            with self.subTest(kind=kind), patch.object(spotify, "request_json", return_value={
                "items": [None, entry], "next": "https://example.invalid",
            }) as request:
                page = spotify.perform("library", (kind, "20"), "test")
                self.assertEqual(page["next"], 40)
                self.assertEqual(page["items"][0]["url"],
                                 "https://open.spotify.com/" + singular + "/" + "a" * 22)
                self.assertEqual(page["items"][0]["uri"], "spotify:" + singular + ":" + "a" * 22)
                self.assertTrue(page["items"][0]["title"].startswith("<"))
                request.assert_called_once_with(spotify.API_URL + "/me/" + kind + "?limit=20&offset=20",
                                                token="test", limit=spotify.LIBRARY_RESPONSE_LIMIT)

    def test_playlist_tracks_keep_their_own_art_and_order(self):
        playlist = "spotify:playlist:" + "p" * 22
        track = {"id": "t" * 22, "type": "track", "name": "Song", "duration_ms": 120000,
                 "album": {"images": [{"width": 64, "url": "https://i.scdn.co/image/aaa"},
                                      {"width": 640, "url": "https://i.scdn.co/image/ccc"},
                                      {"width": 300, "url": "https://i.scdn.co/image/bbb"}]}}
        with patch.object(spotify, "request_json", side_effect=[
            {"id": "p" * 22, "name": "Playlist", "owner": {"display_name": "Owner"}},
            {"items": [{"item": None}, {"item": track}, {"track": dict(track, name="Second")}],
             "next": "https://example.invalid"},
        ]) as request:
            result = spotify.perform("collection", (playlist, "20"), "test")
        self.assertEqual(result["collection"]["subtitle"], "Owner")
        self.assertEqual([item["title"] for item in result["items"]], ["Song", "Second"])
        self.assertEqual(result["items"][0]["art"], "https://i.scdn.co/image/bbb")
        self.assertEqual(result["items"][0]["artHigh"], "https://i.scdn.co/image/ccc")
        self.assertEqual(result["items"][0]["duration"], 120000)
        self.assertEqual(result["next"], 40)
        self.assertEqual(request.call_args.args[0],
                         spotify.API_URL + "/playlists/" + "p" * 22 + "/items?limit=20&offset=20")

    def test_album_tracks_inherit_album_art(self):
        uri = "spotify:album:" + "a" * 22
        art = "https://i.scdn.co/image/abc"
        with patch.object(spotify, "request_json", return_value={
            "id": "a" * 22, "name": "Album", "images": [{"width": 640, "url": art}],
            "tracks": {"items": [{"id": "t" * 22, "name": "First"}, {"id": "u" * 22, "name": "Second"}]},
        }) as request:
            result = spotify.perform("collection", (uri, "0"), "test")
        self.assertEqual([item["title"] for item in result["items"]], ["First", "Second"])
        self.assertEqual(result["items"][0]["art"], art)
        request.assert_called_once()

    def test_widget_launch_uses_an_argument_vector_without_oauth(self):
        uri = "spotify:album:" + "a" * 22
        with patch.object(spotify.shutil, "which", return_value="/test/omarchy-spotify"), patch.object(
            spotify.subprocess, "Popen"
        ) as start, patch.object(spotify, "load_client_id") as client_id, patch.object(
            spotify.select, "select", return_value=([True], [], [])
        ):
            start.return_value.stdout.__enter__.return_value.readline.return_value = b"1\n"
            self.assertEqual(spotify.main("desktop", "--open-album", uri), {"status": "ok"})
            self.assertEqual(start.call_args.args[0], ["/test/omarchy-spotify", "--open-album", uri])
            self.assertNotIn("shell", start.call_args.kwargs)
            self.assertEqual(spotify.main("desktop", uri + "\n"), {"status": "desktop_failed"})
            self.assertEqual(spotify.main("desktop", "--play-track", uri), {"status": "desktop_failed"})
            self.assertEqual(start.call_count, 1)
            start.return_value.stdout.__enter__.return_value.readline.return_value = b""
            self.assertEqual(spotify.main("desktop", "--activate"), {"status": "desktop_failed"})
            client_id.assert_not_called()

    def test_desktop_missing_executable(self):
        with patch.object(spotify.shutil, "which", return_value=None):
            self.assertEqual(spotify.main("desktop", "--activate"), {"status": "desktop_missing"})

    def test_track_metadata_uses_existing_normalization(self):
        full = {"id": "a" * 22, "name": "Song", "album": {"name": "Album", "images": [
            {"url": "https://i.scdn.co/image/111", "width": 300},
            {"url": "https://i.scdn.co/image/222", "width": 640},
        ]}}
        with patch.object(spotify, "request_json", return_value=full):
            result = spotify.perform("track", ("a" * 22,), "test")
        self.assertEqual(result["track"]["album"], "Album")
        self.assertEqual(result["track"]["artHigh"], "https://i.scdn.co/image/222")

    def test_invalid_collection_uri_makes_no_api_call(self):
        with patch.object(spotify, "request_json") as request:
            result = spotify.perform("collection", ("spotify:album:../../", "0"), "test")
        self.assertEqual(result, {"status": "action_failed"})
        request.assert_not_called()

    def test_refresh_preserves_new_permissions_when_scope_is_omitted(self):
        with TemporaryDirectory() as folder, patch.object(
            spotify, "TOKEN_FILE", Path(folder) / "tokens.json"
        ), patch.object(spotify, "request_json", return_value={"access_token": "new", "expires_in": 3600}):
            old = {"refresh_token": "refresh", "scope": "user-library-read"}
            result = spotify.refresh_tokens(old, CLIENT_ID)
            self.assertEqual(result["scope"], old["scope"])

    def test_rejected_token_uses_session_already_refreshed_by_other_process(self):
        with TemporaryDirectory() as folder, patch.object(
            spotify, "TOKEN_FILE", Path(folder) / "tokens.json"
        ), patch.object(spotify, "request_json") as request:
            current = spotify.store_tokens({"access_token": "new", "refresh_token": "refresh",
                                            "expires_in": 3600}, CLIENT_ID)
            self.assertEqual(spotify.session_tokens(CLIENT_ID, rejected="old"), current)
            request.assert_not_called()

    def test_client_id_is_loaded_before_authorization(self):
        with TemporaryDirectory() as folder, patch.object(
            spotify, "CLIENT_ID_FILE", Path(folder) / "spotify-client-id"
        ), patch.object(spotify, "connect") as connect:
            self.assertEqual(spotify.main("status"), {"status": "setup_required"})
            spotify.CLIENT_ID_FILE.write_text("invalid id\n", encoding="utf-8")
            self.assertEqual(spotify.main("connect"), {"status": "setup_required"})
            connect.assert_not_called()
            spotify.CLIENT_ID_FILE.write_text(f"{CLIENT_ID}\n", encoding="utf-8")
            spotify.main("connect")
            connect.assert_called_once_with(CLIENT_ID)

    def test_callback_rejects_wrong_state_and_duplicated_code(self):
        for query in ("state=wrong&code=test", "state=%C3%A9&code=test",
                      "state=expected&code=test&code=extra"):
            self.assertIsNone(spotify.callback_result("/callback?" + query, "expected"))
        self.assertEqual(spotify.callback_result("/callback?state=expected&code=test", "expected"),
                         ("test", ""))
        self.assertEqual(
            spotify.callback_result("/callback?state=expected&error=access_denied", "expected"),
            ("", "denied"),
        )
        self.assertIsNone(spotify.callback_result("/other?state=expected&code=test", "expected"))

    def test_metadata_and_private_session(self):
        track = {
            "id": "a" * 22, "type": "track", "name": "<b>Literal</b>",
            "artists": [None, {"name": "Artist\nName"}],
            "duration_ms": 180500,
            "album": {"name": "Album", "images": [
                {"url": "https://example.com/image/aaa"},
                {"url": "https://i.scdn.co/image/abcd"},
            ]},
        }
        sample = {"item": track, "progress_ms": 200000, "is_playing": True,
                  "device": {"name": "Phone"}}
        result = spotify.playback_sample(sample)
        self.assertEqual(result["title"], "<b>Literal</b>")
        self.assertEqual(result["artist"], "Artist Name")
        self.assertEqual(result["progress"], 180500)
        self.assertEqual(result["art"], "https://i.scdn.co/image/abcd")
        self.assertEqual(result["device"], "Phone")
        self.assertEqual(result["url"], "https://open.spotify.com/track/" + "a" * 22)
        sample["progress_ms"] = None
        self.assertIsNone(spotify.playback_sample(sample)["progress"])
        sample["device"]["is_private_session"] = True
        self.assertEqual(spotify.playback_sample(sample), {"status": "idle"})
        self.assertEqual(spotify.playback_sample({"item": None}), {"status": "idle"})

    def test_token_storage_and_refresh(self):
        with TemporaryDirectory() as folder, patch.object(
            spotify, "TOKEN_FILE", Path(folder) / "private" / "tokens.json"
        ):
            saved = spotify.store_tokens({"access_token": "old", "refresh_token": "refresh",
                                          "expires_in": 3600}, CLIENT_ID)
            self.assertEqual(spotify.TOKEN_FILE.stat().st_mode & 0o777, 0o600)
            self.assertEqual(spotify.TOKEN_FILE.parent.stat().st_mode & 0o777, 0o700)
            with patch.object(spotify, "request_json", side_effect=[
                HTTPError("", 401, "", {}, io.BytesIO()),
                {"access_token": "new", "expires_in": 3600},
                {},
            ]) as request:
                self.assertEqual(spotify.account_operation(CLIENT_ID, "status", ()), {"status": "idle", "extended": False})
                self.assertEqual(request.call_count, 3)
                self.assertEqual(request.call_args_list[1].kwargs["form"]["client_id"], CLIENT_ID)
            self.assertEqual(spotify.load_tokens()["refresh_token"], saved["refresh_token"])
            self.assertEqual(spotify.load_tokens()["access_token"], "new")

    def test_legacy_tokens_are_refreshed_before_use_and_bound_to_client(self):
        with TemporaryDirectory() as folder, patch.object(
            spotify, "TOKEN_FILE", Path(folder) / "tokens.json"
        ), patch.object(spotify, "request_json", side_effect=[
            {"access_token": "new", "expires_in": 3600}, {},
        ]) as request:
            spotify.TOKEN_FILE.write_text(json.dumps({
                "access_token": "old", "refresh_token": "refresh", "expires_at": 2**40,
            }), encoding="utf-8")
            self.assertEqual(spotify.account_operation(CLIENT_ID, "status", ()), {"status": "idle", "extended": False})
            self.assertEqual(request.call_args_list[0].kwargs["form"]["client_id"], CLIENT_ID)
            self.assertEqual(spotify.load_tokens()["client_id"], CLIENT_ID)
            request.reset_mock()
            self.assertEqual(spotify.account_operation("b" * 32, "status", ()), {"status": "disconnected"})
            request.assert_not_called()

    def test_backoff_and_disconnected_result(self):
        body = json.dumps({"error": {"reason": "QUOTA_EXCEEDED"}}).encode()
        error = HTTPError("", 429, "", {"Retry-After": "6000"}, io.BytesIO(body))
        self.assertEqual(spotify.http_failure(error), {"status": "quota", "retry": 6000})
        error = HTTPError("", 429, "", {"Retry-After": "90"}, io.BytesIO())
        self.assertEqual(spotify.http_failure(error), {"status": "limited", "retry": 90})
        with patch.object(spotify, "load_tokens", return_value=None):
            self.assertEqual(spotify.account_operation(CLIENT_ID, "status", ()), {"status": "disconnected"})


if __name__ == "__main__":
    unittest.main()
