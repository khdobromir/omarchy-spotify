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
               XDG_STATE_HOME=directory, XDG_CACHE_HOME=directory, QT_QPA_PLATFORM="offscreen")
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
        interval: 2000
        running: true
        onTriggered: Qt.quit()
    }
}
""")
    fixture = subprocess.Popen([sys.executable, str(Path(__file__).with_name("fixture.py"))], env=env)
    try:
        for _ in range(200):
            if (config / "omarchy-spotify/core.sock").exists():
                break
            time.sleep(0.01)
        result = subprocess.run([sys.argv[1], "-p", directory], env=env, capture_output=True,
                                text=True, timeout=12)
        log = config / "played"
        assert result.returncode == 0 and log.exists(), result.stdout + result.stderr
        assert [json.loads(line) for line in log.read_text().splitlines()] == [["pause", None, "phone"]]
        assert not (config / "omarchy-spotify.lock").exists()
    finally:
        fixture.terminate()
        fixture.wait(timeout=3)
