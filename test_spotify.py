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
                self.assertEqual(spotify.get_playback(CLIENT_ID), {"status": "idle"})
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
            self.assertEqual(spotify.get_playback(CLIENT_ID), {"status": "idle"})
            self.assertEqual(request.call_args_list[0].kwargs["form"]["client_id"], CLIENT_ID)
            self.assertEqual(spotify.load_tokens()["client_id"], CLIENT_ID)
            request.reset_mock()
            self.assertEqual(spotify.get_playback("b" * 32), {"status": "disconnected"})
            request.assert_not_called()

    def test_backoff_and_disconnected_result(self):
        body = json.dumps({"error": {"reason": "QUOTA_EXCEEDED"}}).encode()
        error = HTTPError("", 429, "", {"Retry-After": "6000"}, io.BytesIO(body))
        self.assertEqual(spotify.http_failure(error), {"status": "quota", "retry": 6000})
        error = HTTPError("", 429, "", {"Retry-After": "90"}, io.BytesIO())
        self.assertEqual(spotify.http_failure(error), {"status": "limited", "retry": 90})
        with patch.object(spotify, "load_tokens", return_value=None):
            self.assertEqual(spotify.get_playback(CLIENT_ID), {"status": "disconnected"})


if __name__ == "__main__":
    unittest.main()
