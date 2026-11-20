import asyncio
import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import spotify
from spotify_core import Core, serve

PHONE = {"device": {"id": "phone", "name": "Phone", "is_active": True},
         "item": {"id": "a" * 22, "name": "Song", "type": "track", "duration_ms": 1000},
         "progress_ms": 10, "is_playing": True}


class CoreTest(unittest.IsolatedAsyncioTestCase):
    async def test_two_clients_share_a_phone_and_reconnect(self):
        calls = []

        def api(url, **options):
            calls.append((url, options))
            return PHONE

        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(spotify, "load_client_id", return_value="client"),
            patch.object(spotify, "session_tokens", return_value={
                "access_token": "secret", "scope": " ".join(spotify.SCOPES)}),
            patch.object(spotify, "request_json", side_effect=api),
        ):
            path = Path(directory) / "core.sock"
            core = Core()
            server = await serve(core, path)
            async with server:
                first = await asyncio.open_unix_connection(path)
                second = await asyncio.open_unix_connection(path)

                async def request(client, method, params=None):
                    reader, writer = client
                    writer.write(json.dumps({"version": 1, "id": method,
                                             "method": method, "params": params or {}}).encode() + b"\n")
                    while True:
                        message = json.loads(await asyncio.wait_for(reader.readline(), 3))
                        if message.get("id") == method:
                            return message

                try:
                    await request(first, "subscribe")
                    await request(second, "subscribe")
                    result = await request(first, "playback.pause", {"deviceId": "phone"})
                    self.assertTrue(result["result"]["accepted"])
                    self.assertTrue(any("/pause?device_id=phone" in url for url, _ in calls))
                    self.assertFalse(any(url == spotify.PLAYER_URL and opts.get("method") == "PUT"
                                         for url, opts in calls))
                    result = await request(second, "playback.next", {"deviceId": "laptop"})
                    self.assertEqual(result["error"]["code"], "target_changed")
                    self.assertNotIn("secret", str(await request(second, "snapshot")))
                finally:
                    for _, writer in (first, second):
                        writer.close()
                        await writer.wait_closed()
                    await core.close()

    async def test_no_target_does_not_send_a_write(self):
        core = Core()
        core.sample = {"sync": "ok"}
        core.sampled = time.monotonic()
        with patch.object(core, "refresh"), patch.object(core, "api") as api:
            with self.assertRaisesRegex(ValueError, "no_device"):
                await core.dispatch("playback.resume", {})
            api.assert_not_called()

    async def test_context_and_repeat_target_validation(self):
        core = Core()
        album = "spotify:album:" + "b" * 22
        track = "spotify:track:" + "a" * 22
        calls = []

        def api(method, args):
            calls.append((method, args))
            return spotify.remote_sample(PHONE) if method == "sample" else {"status": "ok"}

        with patch.object(core, "api", side_effect=api):
            await core.dispatch("playback.play", {"uri": track, "context": album, "offset": 22})
            self.assertEqual(calls[-1], ("player", ("/play", {"device_id": "phone"}, "PUT",
                                                    {"context_uri": album, "offset": {"position": 22}})))
            with self.assertRaisesRegex(ValueError, "invalid_uri"):
                await core.dispatch("playback.play", {"uri": "https://example.com"})
            self.assertEqual(sum(method == "player" for method, _ in calls), 1)

    async def test_pages_are_reused_then_invalidated(self):
        core = Core()
        args = {"args": ["tracks", "20"]}
        with patch.object(core, "api", return_value={"status": "ok", "saved": True}) as api:
            await core.dispatch("library", args)
            await core.dispatch("library", args)
            self.assertEqual(api.call_count, 1)
            await core.dispatch("save", {"args": ["a" * 22]})
            await core.dispatch("library", args)
            self.assertEqual(api.call_count, 3)
            api.return_value = {"status": "disconnected"}
            await core.dispatch("disconnect", {})
            self.assertFalse(core.cache)
            self.assertEqual(core.snapshot()["auth"], "disconnected")

    async def test_quota_suspends_api_calls(self):
        core = Core()
        with patch.object(core, "api", return_value={"status": "quota", "retry": 900}) as api:
            await core.dispatch("devices", {})
            self.assertEqual(core.snapshot()["sync"], "quota")
            self.assertEqual(await core.dispatch("queue", {}), {"status": "quota"})
            self.assertEqual(api.call_count, 1)

    async def test_seek_refuses_changed_track_and_clamps_valid_position(self):
        core = Core()
        with patch.object(core, "api", side_effect=lambda method, _: spotify.remote_sample(PHONE)
                          if method == "sample" else {"status": "ok"}) as api:
            with self.assertRaisesRegex(ValueError, "target_changed"):
                await core.dispatch("playback.seek", {"uri": "spotify:track:" + "b" * 22, "position": 2000})
            await core.dispatch("playback.seek", {"uri": "spotify:track:" + "a" * 22, "position": 2000})
            self.assertEqual(api.call_args.args[1][1], {"device_id": "phone", "position_ms": 999})

    async def test_invalid_frames_do_not_break_other_clients(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "core.sock"
            core = Core()
            server = await serve(core, path)
            async with server:
                invalid, sender = await asyncio.open_unix_connection(path)
                good, receiver = await asyncio.open_unix_connection(path)
                try:
                    sender.write(b"[]\n")
                    bad = json.loads(await asyncio.wait_for(invalid.readline(), 2))
                    self.assertEqual(bad["error"]["code"], "invalid_request")
                    sender.write(b"x" * (256 * 1024 + 1))
                    self.assertEqual(await asyncio.wait_for(invalid.read(), 2), b"")
                    receiver.write(b'{"version":1,"id":"1","method":"snapshot"}\n')
                    self.assertTrue(json.loads(await asyncio.wait_for(good.readline(), 2))["ok"])
                finally:
                    sender.close()
                    receiver.close()
                    await core.close()
