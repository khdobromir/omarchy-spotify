"""Single Spotify session and Unix-socket server."""
import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import subprocess
import sys
import time
from tempfile import NamedTemporaryFile
from urllib.error import HTTPError

import spotify

LIMIT = 256 * 1024
READ_METHODS = {"library", "collection", "track", "episode", "favorite", "devices", "queue"}
ACCOUNT_METHODS = {"connect", "disconnect", "save", "remove"}
URI = re.compile(r"spotify:(track|episode|album|playlist):[a-zA-Z0-9]{22}")
TRANSPORT = {
    "resume": ("/play", "PUT", "resuming"),
    "pause": ("/pause", "PUT", "pausing"),
    "next": ("/next", "POST", "skipping_next"),
    "previous": ("/previous", "POST", "skipping_prev"),
    "seek": ("/seek", "PUT", "seeking"),
    "play": ("/play", "PUT", "interrupting_playback"),
    "shuffle": ("/shuffle", "PUT", "toggling_shuffle"),
    "repeat": ("/repeat", "PUT", ""),
    "volume": ("/volume", "PUT", ""),
}


ACTIVE_POLL_INTERVAL = 4
TRACK_COMMANDS = {"next", "previous", "play"}
RECHECK_DELAYS = (0, 0.6, 1.5)


def rewound(before, after):
    start, end = before.get("progress"), after.get("progress")
    return start is not None and end is not None and start - end > 250


@dataclass
class PendingPlayback:
    action: str
    before: dict
    accepted_at: float
    frozen_position: int | None = None
    track_uri: str | None = None
    checks: int = 0

    def confirmed(self, sample):
        if self.action in TRACK_COMMANDS | {"pause", "resume"} and sample.get("deviceId") != self.before.get("deviceId"):
            return False
        if self.action in TRACK_COMMANDS:
            uri = sample.get("uri")
            return bool(uri and (not self.track_uri or uri == self.track_uri)
                        and (uri != self.before.get("uri") or rewound(self.before, sample)))
        if self.action in ("pause", "resume"):
            return sample.get("playing") is (self.action == "resume")
        return True


class Core:
    def __init__(self):
        self.sample = {"status": "loading"}
        self.sampled = 0
        self.revision = 0
        self.account_revision = 0
        self.clients = set()
        self.connections = set()
        self.lock = asyncio.Lock()
        self.cache = OrderedDict()
        self.cooldown = 0
        self.poll_backoff = 0
        self.quota = False
        self.poller = None
        self.wake = asyncio.Event()
        self.pending = None
        self.preview = None
        self.command_unconfirmed = False
        self.end_probes = 0
        self.preferred_path = spotify.CLIENT_ID_FILE.parent / "spotify-preferred-device.json"
        self.preferred = self.load_preference()

    def load_preference(self):
        try:
            with self.preferred_path.open("rb") as stream:
                return spotify.record(spotify.read_json(stream, 2048))
        except (OSError, ValueError):
            return {}

    @staticmethod
    def api(method, args):
        if method in ("connect", "disconnect"):
            return spotify.main(method, *args)
        client = spotify.load_client_id()
        if not client:
            return {"status": "setup_required"}
        try:
            return spotify.account_operation(client, method, args)
        except HTTPError as error:
            if method == "player" and error.code in (403, 404):
                error.close()
                return {"status": "no_device" if error.code == 404 else "action_forbidden"}
            return spotify.http_failure(error)
        except (OSError, ValueError, KeyError, TypeError):
            return {"status": "unavailable"}

    async def call(self, method, args=()):
        if method not in ("disconnect", "connect"):
            if self.quota:
                return {"status": "quota"}
            left = self.cooldown - time.monotonic()
            if left > 0:
                return {"status": "limited", "retry": int(left) + 1}
        result = await asyncio.to_thread(self.api, method, args)
        if result.get("status") == "quota":
            self.quota = True
            self.pending = None
            self.command_unconfirmed = False
            self.end_probes = 0
            self.sample = {**self.sample, "status": "quota", "sync": "quota"}
            self.publish()
        if result.get("status") == "limited":
            self.cooldown = time.monotonic() + result["retry"]
        return result

    def snapshot(self):
        snapshot = {**self.sample, "revision": self.revision, "accountRevision": self.account_revision,
                    "sampleAgeMs": int((time.monotonic() - self.sampled) * 1000) if self.sampled else 0}
        preview = self.preview
        if (preview and (self.sample.get("sync") != "ok"
                         or preview.before.get("deviceId") != self.sample.get("deviceId"))):
            preview = None
        presentation = preview or self.pending
        action = presentation.action if presentation else None
        frozen = presentation.frozen_position if presentation else None
        if frozen is not None:
            snapshot.update(progress=frozen, sampleAgeMs=0)
        at_end = not action and self.end_probes > 0 and self.sample.get("sync") == "ok"
        return {**snapshot, "pending": action, "positionFrozen": frozen is not None,
                "transitioning": action in TRACK_COMMANDS or at_end,
                "commandSync": "unconfirmed" if self.command_unconfirmed and not preview else None,
                "sync": self.sample.get("sync", "loading")}

    def end_delay(self, now):
        duration, position = self.sample.get("duration"), self.sample.get("progress")
        if (self.sample.get("sync") != "ok" or not self.sample.get("playing") or
                not duration or position is None or self.end_probes >= 2):
            return None
        delay = 0.8 if self.end_probes else (duration - position) / 1000 + 0.2
        return self.sampled + delay - now

    def poll_delay(self):
        if self.quota or not self.clients:
            return None
        now = time.monotonic()
        if self.pending:
            delay = self.pending.accepted_at + RECHECK_DELAYS[self.pending.checks] - now
        else:
            delay = ACTIVE_POLL_INTERVAL if self.sample.get("availability") == "active" else 25
            if (end := self.end_delay(now)) is not None:
                delay = min(delay, end)
        return max(delay, self.poll_backoff, self.cooldown - now, 0)

    def publish(self, event="playback.changed", data=None):
        self.revision += 1
        for writer in tuple(self.clients):
            self.send(writer, {"event": event, "revision": self.revision,
                               "data": self.snapshot() if data is None else data})

    @staticmethod
    def send(writer, message):
        if writer.is_closing():
            return
        frame = json.dumps({"version": 1, **message}, ensure_ascii=True, allow_nan=False).encode() + b"\n"
        if len(frame) > LIMIT or writer.transport.get_write_buffer_size() + len(frame) > 2 * LIMIT:
            writer.close()
        else:
            writer.write(frame)

    async def refresh(self):
        end = self.end_delay(time.monotonic())
        if end is not None and end <= 0.001:
            self.end_probes += 1
        result = await self.call("sample")
        status = result["status"]
        valid = status in ("track", "idle", "private", "unknown")
        now = time.monotonic()
        previous_timestamp, timestamp = self.sample.get("timestamp"), result.get("timestamp")
        outdated = (valid and self.sample.get("deviceId") == result.get("deviceId")
                    and previous_timestamp is not None and timestamp is not None and timestamp < previous_timestamp)
        if self.pending:
            self.pending.checks += 1
            if valid and not outdated and self.pending.confirmed(result):
                self.pending = None
            elif (not valid or result.get("deviceId") != self.pending.before.get("deviceId")
                  or self.pending.checks >= len(RECHECK_DELAYS) or now - self.pending.accepted_at > 3):
                self.pending = None
                self.command_unconfirmed = True
        elif not outdated:
            self.command_unconfirmed = False

        if valid and not outdated:
            if (not result.get("playing") or result.get("uri") != self.sample.get("uri")
                    or result.get("deviceId") != self.sample.get("deviceId") or rewound(self.sample, result)):
                self.end_probes = 0
            self.sample = {**result, "auth": "connected", "sync": "ok"}
            self.sampled = now
        elif status in ("disconnected", "setup_required", "reauthorize"):
            if self.sample.get("auth") == "connected":
                self.account_revision += 1
                self.cache.clear()
            self.sample = {"status": status, "auth": status, "sync": status}
            self.sampled = 0
            self.end_probes = 0
        elif not valid:
            self.sample = {**self.sample, "status": status, "sync": status}
        self.poll_backoff = (min(300, max(30, result.get("retry", 0), 2 * self.poll_backoff))
                             if status == "unavailable" else 0)
        self.publish()

    async def poll(self):
        while True:
            async with self.lock:
                self.wake.clear()
                if self.clients and not self.quota:
                    await self.refresh()
            try:
                await asyncio.wait_for(self.wake.wait(), self.poll_delay())
            except TimeoutError:
                pass

    async def close(self):
        if self.poller is not None:
            self.poller.cancel()
            await asyncio.gather(self.poller, return_exceptions=True)
        for writer in tuple(self.connections):
            writer.transport.abort()

    def estimated_position(self, now):
        position = self.sample.get("progress")
        if position is not None and self.sample.get("playing"):
            position += int(max(0, now - self.sampled) * 1000)
            position = min(position, self.sample.get("duration") or position)
        return position

    def preview_command(self, method, params):
        action = method.removeprefix("playback.")
        target = self.sample.get("deviceId")
        if (self.preview or not method.startswith("playback.") or action not in TRACK_COMMANDS | {"pause"}
                or self.sample.get("sync") != "ok" or not target or params.get("deviceId", target) != target):
            return None
        now = time.monotonic()
        position = self.estimated_position(now) if action == "pause" else None
        self.preview = PendingPlayback(action, self.sample, now, frozen_position=position)
        self.publish()
        return self.preview

    async def execute(self, method, params):
        preview = self.preview_command(method, params)
        try:
            try:
                await asyncio.wait_for(self.lock.acquire(), 4)
            except TimeoutError:
                raise ValueError("request_timeout") from None
            try:
                if preview is None:
                    preview = self.preview_command(method, params)
                return await self.dispatch(method, params, preview=preview)
            finally:
                self.lock.release()
        finally:
            if preview is not None and self.preview is preview:
                self.preview = None
                self.publish()

    async def player(self, action, params, preview=None):
        if action not in TRANSPORT and action != "transfer":
            raise ValueError("unsupported_method")
        if action == "transfer":
            target = spotify.device_id(params.get("deviceId"))
            devices = await self.call("devices")
            if devices["status"] != "ok":
                raise ValueError(devices["status"])
            if not target or not any(d["id"] == target and not d["restricted"] for d in devices["devices"]):
                raise ValueError("no_device")
            suffix, query, method, body = "", {}, "PUT", {"device_ids": [target], "play": False}
        else:
            await self.refresh()
            if self.sample.get("sync") != "ok":
                raise ValueError(self.sample.get("sync", "unavailable"))
            target = self.sample.get("deviceId")
            capabilities = self.sample.get("capabilities", {})
            if not target or not capabilities.get("active"):
                raise ValueError("no_device")
            if params.get("deviceId", target) != target:
                raise ValueError("target_changed")
            suffix, method, disallow = TRANSPORT[action]
            if capabilities.get("restricted") or capabilities.get("private") or disallow in self.sample["disallows"]:
                raise ValueError("action_forbidden")
            query, body = {"device_id": target}, None
            if action == "seek":
                position = spotify.milliseconds(params.get("position"))
                if position is None or not params.get("uri"):
                    raise ValueError("invalid_params")
                if (self.sample.get("sync") != "ok" or self.sample.get("uri") != params["uri"]
                        or self.sample.get("deviceId") != target or not self.sample.get("duration")):
                    raise ValueError("target_changed")
                if "seeking" in self.sample["disallows"] or self.sample["capabilities"]["restricted"]:
                    raise ValueError("action_forbidden")
                query["position_ms"] = min(position, self.sample["duration"] - 1)
            elif action == "play":
                uri = params.get("uri")
                if not isinstance(uri, str) or not URI.fullmatch(uri):
                    raise ValueError("invalid_uri")
                kind = uri.split(":")[1]
                context = params.get("context")
                if context is not None:
                    if (not isinstance(context, str) or not URI.fullmatch(context)
                            or context.split(":")[1] not in ("album", "playlist") or kind not in ("track", "episode")):
                        raise ValueError("invalid_uri")
                    offset = params.get("offset")
                    if type(offset) is not int or not 0 <= offset <= spotify.MAX_OFFSET:
                        raise ValueError("invalid_params")
                    body = {"context_uri": context, "offset": {"position": offset}}
                else:
                    body = {"uris": [uri]} if kind in ("track", "episode") else {"context_uri": uri}
            elif action == "shuffle":
                if type(params.get("state")) is not bool:
                    raise ValueError("invalid_params")
                query["state"] = str(params["state"]).lower()
            elif action == "repeat":
                state = params.get("state")
                if state not in ("off", "context", "track"):
                    raise ValueError("invalid_params")
                if ("toggling_repeat_" + ("track" if state == "track" else "context")) in self.sample["disallows"]:
                    raise ValueError("action_forbidden")
                query["state"] = state
            elif action == "volume":
                value = params.get("volume")
                if type(value) is not int or not 0 <= value <= 100:
                    raise ValueError("invalid_params")
                if not capabilities.get("supportsVolume"):
                    raise ValueError("action_forbidden")
                query["volume_percent"] = value
        result = await self.call("player", (suffix, query, method, body))
        if result["status"] != "ok":
            self.wake.set()
            raise ValueError(result["status"])
        now = time.monotonic()
        position = preview.frozen_position if preview else self.estimated_position(now)
        track_uri = params.get("uri") if action == "play" else None
        if track_uri and not track_uri.startswith(("spotify:track:", "spotify:episode:")):
            track_uri = None
        self.pending = PendingPlayback(action, self.sample, now,
                                       frozen_position=position if action == "pause" else None,
                                       track_uri=track_uri)
        if self.preview is preview:
            self.preview = None
        self.command_unconfirmed = False
        self.publish()
        self.wake.set()
        return {"accepted": True}

    async def dispatch(self, method, params, *, preview=None):
        if method == "snapshot":
            return self.snapshot()
        if method.startswith("playback."):
            return await self.player(method.removeprefix("playback."), params, preview)
        if method not in READ_METHODS | ACCOUNT_METHODS | {"preferDevice", "refresh"}:
            raise ValueError("unsupported_method")
        if method == "refresh":
            if self.quota:
                raise ValueError("quota")
            self.wake.set()
            return {"accepted": True}
        if method == "preferDevice":
            devices = await self.call("devices")
            target = params.get("deviceId")
            match = next((d for d in devices.get("devices", ()) if d["id"] == target and target), None)
            if not match:
                raise ValueError(devices["status"] if devices["status"] != "ok" else "no_device")
            self.preferred = {key: match[key] for key in ("id", "name", "type")}
            self.preferred_path.parent.mkdir(parents=True, exist_ok=True)
            with NamedTemporaryFile(mode="w", dir=self.preferred_path.parent, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    json.dump(self.preferred, stream)
                    stream.flush()
                    temporary.replace(self.preferred_path)
                finally:
                    temporary.unlink(missing_ok=True)
            self.cache.pop(("devices", ()), None)
            return {"status": "ok"}
        args = params.get("args", [])
        lengths = {"library": 2, "collection": 2, "track": 1, "episode": 1,
                   "favorite": 1, "save": 1, "remove": 1}
        if (not isinstance(args, list) or len(args) != lengths.get(method, 0)
                or any(not isinstance(arg, str) or len(arg) > 128 for arg in args)):
            raise ValueError("invalid_params")
        if method == "collection" and (not URI.fullmatch(args[0]) or args[0].split(":")[1] not in ("album", "playlist")):
            raise ValueError("invalid_uri")
        if method in ("track", "episode", "favorite", "save", "remove") and not spotify.item_id(args[0]):
            raise ValueError("invalid_uri")
        key = (method, tuple(args))
        now = time.monotonic()
        if key in self.cache and self.cache[key][0] > now:
            self.cache.move_to_end(key)
            return self.cache[key][1]
        result = await self.call(method, args)
        status = result["status"]
        if method == "devices" and status == "ok":
            for device in result["devices"]:
                device["preferred"] = bool(device["id"] and all(device[k] == self.preferred.get(k)
                                                                for k in ("id", "name", "type")))
        if method in READ_METHODS and status in ("ok", "collection_forbidden"):
            self.cache[key] = (now + (5 if method == "queue" else 15 if method == "devices" else 120), result)
            while len(self.cache) > 64:
                self.cache.popitem(last=False)
        if method in ("connect", "disconnect") and status in ("authorized", "disconnected"):
            self.cache.clear()
            self.account_revision += 1
            self.sample = {"status": "loading" if status == "authorized" else status, "auth": status}
            self.sampled = 0
            self.pending = None
            self.command_unconfirmed = False
            self.end_probes = 0
            self.publish()
            self.wake.set()
        if method in ("save", "remove") and status == "ok":
            for cached in list(self.cache):
                if cached == ("favorite", tuple(args)) or cached[0] == "library" and cached[1][0] == "tracks":
                    del self.cache[cached]
            self.publish("library.changed", {"trackId": args[0], "saved": result["saved"]})
        return result

    async def client(self, reader, writer):
        if len(self.connections) >= 16:
            writer.close()
            return
        self.connections.add(writer)
        seen = OrderedDict()
        try:
            while frame := await asyncio.wait_for(reader.readline(), None if writer in self.clients else 10):
                identity = None
                try:
                    message = json.loads(frame)
                    if not isinstance(message, dict):
                        raise ValueError("invalid_request")
                    identity = message.get("id")
                    if not isinstance(identity, str) or not 0 < len(identity) <= 64:
                        identity = None
                        raise ValueError("invalid_request")
                    method, params = message.get("method"), message.get("params", {})
                    if type(message.get("version")) is not int or message["version"] != 1:
                        raise ValueError("unsupported_version")
                    if not isinstance(method, str) or not isinstance(params, dict):
                        raise ValueError("invalid_request")
                    if identity in seen:
                        raise ValueError("duplicate_id")
                    seen[identity] = None
                    if len(seen) > 64:
                        seen.popitem(last=False)
                    if method == "subscribe":
                        first = not self.clients
                        self.clients.add(writer)
                        result = self.snapshot()
                        if first:
                            self.wake.set()
                    else:
                        result = await self.execute(method, params)
                    self.send(writer, {"id": identity, "ok": True, "result": result})
                except (ValueError, RecursionError) as error:
                    code = str(error)
                    if not re.fullmatch(r"[a-z_]{1,40}", code):
                        code = "invalid_request"
                    self.send(writer, {"id": identity, "ok": False, "error": {"code": code}})
                except OSError:
                    self.send(writer, {"id": identity, "ok": False, "error": {"code": "unavailable"}})
                await asyncio.wait_for(writer.drain(), 2)
        except (OSError, ValueError, TimeoutError):
            pass
        finally:
            self.clients.discard(writer)
            self.connections.discard(writer)
            writer.close()


async def serve(core, path=None, listener=None):
    if listener is None:
        server = await asyncio.start_unix_server(core.client, path, limit=LIMIT)
        os.chmod(path, 0o600)
    else:
        # systemd owns the pathname across service restarts.
        server = await asyncio.start_unix_server(core.client, sock=listener, limit=LIMIT, cleanup_socket=False)
    core.poller = asyncio.create_task(core.poll())
    return server


async def main():
    core = Core()
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime or not os.path.isabs(runtime):
        raise SystemExit("XDG_RUNTIME_DIR must be an absolute directory")
    directory = Path(runtime) / "omarchy-spotify"
    path = directory / "core.sock"
    directory.mkdir(mode=0o700, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise SystemExit("Unsafe Spotify Core runtime directory")

    inherited = os.environ.get("LISTEN_PID") == str(os.getpid()) and os.environ.get("LISTEN_FDS") == "1"
    with os.fdopen(os.open(directory / "core.lock", os.O_CREAT | os.O_NOFOLLOW | os.O_WRONLY, 0o600), "wb") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        listener = None
        if inherited:
            listener = socket.socket(fileno=3)
            if listener.family != socket.AF_UNIX or listener.getsockname() != str(path):
                raise SystemExit("Unexpected Spotify Core listening socket")
        elif path.exists():
            try:
                _, writer = await asyncio.open_unix_connection(path)
            except ConnectionRefusedError:
                if not stat.S_ISSOCK(path.lstat().st_mode):
                    raise SystemExit("Invalid Spotify Core socket")
                path.unlink()
            else:
                writer.close()
                return
        os.umask(0o077)
        stopped = asyncio.Event()
        for signum in (signal.SIGTERM, signal.SIGINT):
            asyncio.get_running_loop().add_signal_handler(signum, stopped.set)
        server = await serve(core, path, listener)
        async with server:
            await stopped.wait()
            await core.close()


if __name__ == "__main__":
    if sys.argv[1:] == ["--activate"]:
        subprocess.Popen([sys.executable, "-B", __file__], start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif not sys.argv[1:]:
        asyncio.run(main())
    else:
        raise SystemExit("Usage: omarchy-spotify-core [--activate]")
