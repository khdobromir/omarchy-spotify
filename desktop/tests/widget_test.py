"""The real Quickshell socket client, with no desktop process."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
import time

ROOT = Path(__file__).resolve().parents[2]
with TemporaryDirectory(prefix="spotify-widget-") as directory:
    config = Path(directory)
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, XDG_CONFIG_HOME=directory,
               XDG_STATE_HOME=directory, XDG_CACHE_HOME=directory, QT_QPA_PLATFORM="offscreen",
               PATH=os.defpath)
    shutil.copy(ROOT / "Service.qml", config)
    (config / "shell.qml").write_text("""
import QtQuick
import Quickshell
ShellRoot {
    id: root
    property var spotify
    Component.onCompleted: {
        const service = Qt.createComponent("Service.qml", Component.PreferSynchronous);
        if (service.status !== Component.Ready) {
            console.error(service.errorString());
            Qt.quit();
            return;
        }
        root.spotify = service.createObject(null);
    }
    Connections {
        target: root.spotify ?? null
        function onSampleChanged() {
            const spotify = root.spotify;
            if (spotify.sample.device === "Phone" && spotify.available)
                spotify.run("pause", ["phone"]);
        }
    }
    Timer {
        interval: 6500
        running: true
        onTriggered: Qt.quit()
    }
}
""")
    endpoint = config / "omarchy-spotify/core.sock"
    log = config / "played"

    def start_core():
        return subprocess.Popen([sys.executable, str(Path(__file__).with_name("fixture.py"))], env=env)

    def wait_for(path):
        for _ in range(250):
            if path.exists() and (path == endpoint or path.stat().st_size):
                return
            time.sleep(0.01)
        raise AssertionError(f"Missing {path.name}")

    fixture = start_core()
    widget = None
    try:
        wait_for(endpoint)
        widget = subprocess.Popen([sys.argv[1], "-p", directory], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        wait_for(log)
        fixture.terminate()
        fixture.wait(timeout=3)
        endpoint.unlink()
        time.sleep(1.5)
        fixture = start_core()
        output, _ = widget.communicate(timeout=12)
        assert widget.returncode == 0, output
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        assert commands == [["pause", None, "phone"], ["pause", None, "phone"]], (commands, output)
        assert not (config / "omarchy-spotify.lock").exists()
    finally:
        if widget is not None and widget.poll() is None:
            widget.kill()
            widget.communicate()
        if fixture.poll() is None:
            fixture.terminate()
            fixture.wait(timeout=3)
