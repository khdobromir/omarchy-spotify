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
HELPER = Path(__file__).with_name("fixture.py").read_text()


class RoutingTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory(prefix="spotify-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="",
                        QT_FORCE_STDERR_LOGGING="1",
                        XDG_RUNTIME_DIR=directory.name,
                        XDG_STATE_HOME=directory.name, XDG_CONFIG_HOME=directory.name,
                        XDG_CACHE_HOME=directory.name,
                        PATH=directory.name + ":" + os.environ["PATH"])
        (self.root / "omarchy-spotify").mkdir()
        (self.root / "omarchy-spotify-bin").mkdir()
        (self.root / "omarchy-spotify-bin" / "omarchy-spotify").symlink_to(APP)
        self.env["PATH"] = str(self.root / "omarchy-spotify-bin") + ":" + self.env["PATH"]
        state = self.root / "omarchy-spotify"
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
        app = self.start("--play-track", TRACK)
        self.await_log("played", ["play", TRACK])
        daemon = (self.root / "daemon").read_text()
        for collection in (ALBUM, PLAYLIST):
            result = self.launch("--open-album" if collection == ALBUM else "--open-playlist", collection)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.await_log("web", ["collection", collection, "0"])
        self.assertEqual(self.launch("--activate").returncode, 0)
        self.assertEqual(self.launch("--open-track", TRACK).returncode, 0)
        self.assertEqual(app.poll(), None)
        self.assertEqual((self.root / "daemon").read_text(), daemon)
        self.assertEqual(self.log("played"), [["play", TRACK]])

    def test_widget_launcher_cold_then_warm(self):
        result = subprocess.run([PYTHON, Path(__file__).resolve().parents[2] / "spotify.py",
                                 "desktop", "--play-track", TRACK],
                                env=self.env, capture_output=True, timeout=8)
        self.assertEqual(json.loads(result.stdout), {"status": "ok"})
        self.await_log("played", ["play", TRACK])
        app_pid = int((self.root / "omarchy-spotify.lock").read_text().splitlines()[0])
        self.addCleanup(os.kill, app_pid, 15)
        result = subprocess.run([PYTHON, Path(__file__).resolve().parents[2] / "spotify.py",
                                 "desktop", "--open-track", TRACK],
                                env=self.env, capture_output=True, timeout=8)
        self.assertEqual(json.loads(result.stdout), {"status": "ok"})
        self.assertEqual(self.log("played"), [["play", TRACK]])

    def test_invalid_command_is_rejected(self):
        for command in ("https://example.org", TRACK + "\n", "--unexpected", "spotify:episode:" + "x" * 22):
            with self.subTest(command=command):
                self.assertNotEqual(self.launch(command).returncode, 0)
        self.assertNotEqual(self.launch("--open-track", ALBUM).returncode, 0)
        self.assertNotEqual(self.launch("--play-track", PLAYLIST).returncode, 0)
        self.assertFalse((self.root / "daemon").exists())

    def test_cold_collection_browses_without_playback_and_rejects_invalid_ipc(self):
        self.start("--open-playlist", PLAYLIST)
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

    def test_locked_instance_without_socket_does_not_start_another_player(self):
        self.start()
        self.await_log("web", ["library", "tracks", "0"])
        daemon = (self.root / "daemon").read_text()
        (self.root / "omarchy-spotify.sock").unlink()
        self.assertNotEqual(self.launch("--activate").returncode, 0)
        self.assertEqual((self.root / "daemon").read_text(), daemon)


if __name__ == "__main__":
    unittest.main()
