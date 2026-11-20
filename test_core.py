import asyncio
import json
import socket
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
                    while True:
                        event = json.loads(await asyncio.wait_for(second[0].readline(), 3))
                        if event.get("data", {}).get("pending") == "pause":
                            self.assertTrue(event["data"]["positionFrozen"])
                            break
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

    async def test_inherited_socket_path_survives_core_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "core.sock"
            with socket.socket(socket.AF_UNIX) as manager:
                manager.bind(str(path))
                manager.listen()
                core = Core()
                server = await serve(core, listener=manager.dup())
                async with server:
                    await core.close()
                self.assertTrue(path.is_socket())

    async def test_pause_freezes_both_snapshots_until_spotify_confirms(self):
        core = Core()
        playing = {**spotify.remote_sample(PHONE), "duration": 90000, "progress": 20000}
        clock = 100.0
        with patch("spotify_core.time") as timing, patch.object(
            core, "call", side_effect=[playing, {"status": "ok"}, playing, {**playing, "playing": False}]
        ):
            timing.monotonic.side_effect = lambda: clock
            await core.dispatch("playback.pause", {"deviceId": "phone"})
            accepted = core.snapshot()
            self.assertTrue(accepted["positionFrozen"])
            self.assertEqual(accepted["pending"], "pause")
            clock += 0.3
            await core.refresh()
            self.assertEqual(core.snapshot()["progress"], accepted["progress"])
            self.assertTrue(core.snapshot()["positionFrozen"])
            clock += 0.6
            await core.refresh()
            self.assertFalse(core.snapshot()["playing"])
            self.assertFalse(core.snapshot()["positionFrozen"])
            self.assertIsNone(core.snapshot()["pending"])

    async def test_track_transition_waits_for_new_snapshot(self):
        core = Core()
        old = spotify.remote_sample(PHONE)
        new = {**old, "uri": "spotify:track:" + "b" * 22, "title": "New", "art": "new-art"}
        with patch.object(core, "call", side_effect=[old, {"status": "ok"}, old, new]):
            await core.dispatch("playback.next", {"deviceId": "phone"})
            self.assertTrue(core.snapshot()["transitioning"])
            await core.refresh()
            self.assertTrue(core.snapshot()["transitioning"])
            await core.refresh()
            result = core.snapshot()
            self.assertFalse(result["transitioning"])
            self.assertEqual((result["title"], result["art"]), ("New", "new-art"))

    async def test_track_end_probe_is_bounded_and_rearms_on_repeat(self):
        core = Core()
        core.clients.update((object(), object()))
        old = {**spotify.remote_sample(PHONE), "duration": 90000, "progress": 89000}
        clock = 100.0
        with patch("spotify_core.time") as timing, patch.object(
            core, "call", side_effect=[old, old, old, {**old, "progress": 20}, {**old, "playing": False}]
        ), patch.object(core, "send"):
            timing.monotonic.side_effect = lambda: clock
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 1.2)
            clock += 1.2
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 0.8)
            clock += 0.8
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 4)
            clock += 4
            await core.refresh()
            self.assertEqual(core.end_probes, 0)
            self.assertAlmostEqual(core.poll_delay(), 4)
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 4)
            core.cooldown = clock + 30
            self.assertAlmostEqual(core.poll_delay(), 30)

    async def test_unconfirmed_command_returns_to_last_reported_sample(self):
        core = Core()
        old = {**spotify.remote_sample(PHONE), "duration": 90000, "progress": 20000}
        clock = 100.0
        core.clients.add(object())
        with patch("spotify_core.time") as timing, patch.object(
            core, "call", side_effect=[old, {"status": "ok"}, old, old, old]
        ), patch.object(core, "send"):
            timing.monotonic.side_effect = lambda: clock
            await core.dispatch("playback.next", {})
            self.assertEqual(core.poll_delay(), 0)
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 0.6)
            clock += 0.6
            await core.refresh()
            self.assertAlmostEqual(core.poll_delay(), 0.9)
            clock += 0.9
            await core.refresh()
            result = core.snapshot()
            self.assertEqual(result["commandSync"], "unconfirmed")
            self.assertFalse(result["transitioning"])
            self.assertEqual(result["uri"], old["uri"])
            self.assertEqual(core.poll_delay(), 4)

    async def test_external_pause_and_rate_limit_stop_interpolation(self):
        core = Core()
        playing = {**spotify.remote_sample(PHONE), "duration": 90000, "progress": 20000}
        core.clients.add(object())
        with patch.object(core, "call", side_effect=[
            playing, {**playing, "playing": False, "progress": 20500}, {"status": "limited", "retry": 30}
        ]), patch.object(core, "send"):
            await core.refresh()
            self.assertEqual(core.poll_delay(), 4)
            await core.refresh()
            self.assertFalse(core.snapshot()["playing"])
            self.assertEqual(core.snapshot()["progress"], 20500)
            self.assertEqual(core.poll_delay(), 4)
            await core.refresh()
            self.assertEqual(core.snapshot()["sync"], "limited")

    async def test_older_spotify_sample_does_not_restore_previous_track(self):
        core = Core()
        old = {**PHONE, "timestamp": 1000}
        new = {**PHONE, "timestamp": 2000, "item": {**PHONE["item"], "id": "b" * 22, "name": "New"}}
        with patch.object(core, "call", side_effect=[
            spotify.remote_sample(old), {"status": "ok"}, spotify.remote_sample(new), spotify.remote_sample(old)
        ]):
            await core.dispatch("playback.next", {})
            await core.refresh()
            self.assertEqual(core.snapshot()["title"], "New")
            await core.refresh()
            self.assertEqual(core.snapshot()["title"], "New")

    async def test_pause_is_published_while_command_waits_for_network(self):
        core = Core()
        core.sample = {**spotify.remote_sample(PHONE), "sync": "ok", "duration": 90000, "progress": 20000}
        core.sampled = 100.0
        snapshots = []
        before_command = core.sample

        with patch("spotify_core.time") as timing, patch.object(
            core, "publish", side_effect=lambda: snapshots.append(core.snapshot())
        ):
            async def delayed_spotify(method, args=()):
                timing.monotonic.return_value += 1
                self.assertEqual(core.snapshot()["progress"], 20500)
                self.assertTrue(core.snapshot()["positionFrozen"])
                return before_command if method == "sample" else {"status": "ok"}

            timing.monotonic.return_value = 100.5
            with patch.object(core, "call", side_effect=delayed_spotify) as call:
                task = None
                try:
                    async with core.lock:
                        task = asyncio.create_task(core.execute("playback.pause", {"deviceId": "phone"}))
                        await asyncio.sleep(0)
                        call.assert_not_called()
                        self.assertEqual(snapshots[-1]["progress"], 20500)
                        self.assertTrue(snapshots[-1]["positionFrozen"])
                    self.assertEqual(await task, {"accepted": True})
                    self.assertEqual(core.snapshot()["progress"], 20500)
                    self.assertTrue(core.snapshot()["positionFrozen"])
                finally:
                    if task and not task.done():
                        task.cancel()
                        await asyncio.gather(task, return_exceptions=True)

    async def test_denied_skip_clears_the_transition(self):
        core = Core()
        core.sample = {**spotify.remote_sample(PHONE), "sync": "ok"}
        core.sampled = time.monotonic()
        samples = [core.sample, {"status": "action_forbidden"}]
        with patch.object(core, "publish") as publish, patch.object(core, "call", side_effect=samples):
            snapshots = []
            publish.side_effect = lambda: snapshots.append(core.snapshot())
            with self.assertRaisesRegex(ValueError, "action_forbidden"):
                await core.execute("playback.next", {"deviceId": "phone"})
            self.assertTrue(snapshots[0]["transitioning"])
            self.assertFalse(snapshots[-1]["transitioning"])
            self.assertEqual(snapshots[-1]["uri"], core.sample["uri"])
