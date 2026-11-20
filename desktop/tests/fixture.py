"""Fake Soloist and Spotify helper, selected by executable name."""

import asyncio
import json
import os
from pathlib import Path
import sys
import time

root = Path(os.environ["XDG_STATE_HOME"])
args = sys.argv[1:]


def log(name, values):
    with (root / name).open("a") as output:
        print(json.dumps(values), file=output)


def wait_for_parent(seconds):
    parent = os.getppid()
    for _ in range(seconds * 10):
        if os.getppid() != parent:
            return
        time.sleep(0.1)


def soloist():
    data = Path(args[args.index("--data-dir") + 1])
    if args[0] != "ctl":
        (root / "daemon").write_text(str(os.getpid()))
        (data / "soloist.pid").write_text(str(os.getpid()))
        (data / "ws.port").write_text("8001")
        (data / "ws.addr").write_text("127.0.0.1")
        wait_for_parent(40)
    elif args[1] == "trace":
        time.sleep(0.2)
        print('0 {"type":"auth_state","logged_in":true}', flush=True)
        if (root / "events").exists():
            with (root / "events").open() as events:
                for _ in range(4000):
                    for line in events:
                        print(line, end="", flush=True)
                    time.sleep(0.01)
        else:
            wait_for_parent(40)
    else:
        log("played", args[1:-2])


def web(operation, *parameters):
    log("web", [operation, *parameters])
    if operation in ("track", "episode"):
        identity = parameters[0]
        if identity.startswith("b") and (root / "events").exists():
            time.sleep(1.5)
        if identity.startswith("d"):
            return {"status": "unavailable", "retry": 30}
        if identity.startswith("e"):
            identity = "f" * 22
        return {"status": "ok", "track": {"uri": f"spotify:{operation}:{identity}",
                                         "title": "API " + identity[0], "subtitle": "Artist",
                                         "art": "https://i.scdn.co/image/123",
                                         "artHigh": "https://i.scdn.co/image/456",
                                         "album": "Album", "duration": 23000}}
    if operation == "favorite":
        return {"status": "ok", "saved": False}
    kind, offset = parameters
    time.sleep(0.1)
    collection = {"title": "Test collection", "uri": kind}
    if not (root / "library-fixture").exists():
        return {"status": "ok", "items": [], "collection": collection}
    if kind.startswith("spotify:playlist:") and offset == "20":
        failed = root / "limited"
        if not failed.exists():
            failed.touch()
            return {"status": "limited", "retry": 1}
    if kind == "tracks" and offset == "20":
        time.sleep(0.35)
    values = range(int(offset), 20 if offset == "0" else 23)
    items = [{"uri": "spotify:track:" + "a" * 22, "title": f"{kind} {index}"} for index in values]
    return {"status": "ok", "items": items, "collection": collection,
            "next": 20 if offset == "0" else None}



async def core():
    async def handle(reader, writer):
        try:
            while line := await reader.readline():
                message = json.loads(line)
                method = message["method"]
                params = message["params"]
                if method == "subscribe":
                    result = {"status": "track", "auth": "connected", "sync": "ok", "revision": 1,
                              "accountRevision": 0, "deviceId": "phone", "device": "Phone",
                              "capabilities": {"active": True}, "playing": True,
                              "uri": "spotify:track:" + "t" * 22}
                elif method.startswith("playback."):
                    log("played", [method.split(".")[1], params.get("uri"), params.get("deviceId")])
                    result = {"accepted": True}
                else:
                    result = await asyncio.to_thread(web, method, *params.get("args", []))
                writer.write((json.dumps({"version": 1, "id": message["id"], "ok": True,
                                          "result": result}) + "\n").encode())
                await writer.drain()
        finally:
            writer.close()

    path = Path(os.environ["XDG_RUNTIME_DIR"]) / "omarchy-spotify/core.sock"
    path.parent.mkdir(mode=0o700, exist_ok=True)
    async with await asyncio.start_unix_server(handle, path) as server:
        await server.serve_forever()


if __name__ == "__main__":
    if Path(sys.argv[0]).name == "soloist":
        soloist()
    else:
        asyncio.run(core())
