"""Exercise the real desktop executable with isolated local-player and Web API fixtures."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
import unittest

APP = Path(sys.argv.pop(1)).resolve()
PYTHON = shutil.which("python3")
TRACK = "spotify:track:" + "t" * 22
ALBUM = "spotify:album:" + "a" * 22
PLAYLIST = "spotify:playlist:" + "p" * 22
HELPER = r'''
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ["XDG_STATE_HOME"])
args = sys.argv[1:]
if pathlib.Path(sys.argv[0]).name == "soloist":
    data = pathlib.Path(args[args.index("--data-dir") + 1])
    if args[0] != "ctl":
        (root / "daemon").write_text(str(os.getpid()))
        (data / "soloist.pid").write_text(str(os.getpid()))
        (data / "ws.port").write_text("8001")
        (data / "ws.addr").write_text("127.0.0.1")
        time.sleep(20)
    elif args[1] == "trace":
        time.sleep(0.6)
        print('0 {"type":"auth_state","logged_in":true}', flush=True)
        time.sleep(20)
    else:
        with (root / "played").open("a") as log:
            print(json.dumps(args[1:-2]), file=log)
else:
    with (root / "web").open("a") as log:
        print(json.dumps(args[2:]), file=log)
    time.sleep(0.1)
    print(json.dumps({"status":"ok","items":[],
                      "collection":{"title":"Test collection","uri":args[3]}}))
'''


class RoutingTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory(prefix="spotify-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                        XDG_RUNTIME_DIR=directory.name,
                        XDG_STATE_HOME=directory.name, XDG_CONFIG_HOME=directory.name,
                        XDG_CACHE_HOME=directory.name,
                        PATH=directory.name + ":" + os.environ["PATH"])
        state = self.root / "omarchy-spotify"
        state.mkdir()
        (state / "soloist-key").write_text("fixture")
        for name in ("soloist", "python3"):
            path = self.root / name
            path.write_text("#!" + PYTHON + "\n" + HELPER)
            path.chmod(0o700)

    def start(self, *args):
        process = subprocess.Popen([APP, *args], env=self.env, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)

    def launch(self, *args):
        return subprocess.run([APP, *args], env=self.env, capture_output=True, timeout=5)

    def log(self, name):
        path = self.root / name
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def await_log(self, name, entry):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if entry in self.log(name):
                return
            time.sleep(0.03)
        self.fail(f"{entry!r} missing in {name}: {self.log(name)!r}")

    def test_cold_track_waits_for_soloist_and_collection_launches_only_browse(self):
        app = self.start(TRACK)
        self.await_log("played", ["play", TRACK])
        daemon = (self.root / "daemon").read_text()
        for collection in (ALBUM, PLAYLIST):
            result = self.launch(collection)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.await_log("web", ["collection", collection, "0"])
        self.assertEqual(self.launch().returncode, 0)
        self.assertEqual(app.poll(), None)
        self.assertEqual((self.root / "daemon").read_text(), daemon)
        self.assertEqual(self.log("played"), [["play", TRACK]])

    def test_invalid_command_is_rejected(self):
        for command in ("https://example.org", TRACK + "\n", "--unexpected", "spotify:episode:" + "x" * 22):
            with self.subTest(command=command):
                self.assertNotEqual(self.launch(command).returncode, 0)
        self.assertFalse((self.root / "daemon").exists())

    def test_cold_collection_browses_without_playback_and_rejects_invalid_ipc(self):
        self.start(PLAYLIST)
        self.await_log("web", ["collection", PLAYLIST, "0"])
        sock = self.root / "omarchy-spotify.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(str(sock))
            client.sendall(TRACK.encode() + b"\n\0")
            self.assertEqual(client.recv(1), b"0")
        self.assertEqual(sock.stat().st_mode & 0o077, 0)
        self.assertEqual(self.launch(ALBUM).returncode, 0)
        self.await_log("web", ["collection", ALBUM, "0"])
        self.assertEqual(self.log("played"), [])


if __name__ == "__main__":
    unittest.main()
