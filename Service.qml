import QtQuick
import Quickshell
import Quickshell.Io

Scope {
  id: root

  property var sample: ({ status: "loading" })
  property double sampledAt: 0
  property string action: "status"
  readonly property bool busy: worker.running
  readonly property string helper: Qt.resolvedUrl("spotify.py").toString().slice("file://".length)

  function run(command) {
    if (root.busy)
      return
    poll.stop()
    root.action = command
    worker.running = true
  }

  function finish(text) {
    let result
    try {
      result = JSON.parse(text)
    } catch (error) {
      result = root.action === "status" ? { status: "unavailable", retry: 30 }
        : { status: root.action === "connect" ? "auth_error" : "disconnect_error" }
    }
    root.sampledAt = Date.now()
    root.sample = result
    if (result.status === "authorized") {
      poll.interval = 1
      poll.start()
    } else if (result.retry || result.status === "track" || result.status === "idle") {
      const seconds = result.retry || (result.playing ? 10 : 25)
      poll.interval = Math.min(seconds * 1000, 2147483647)
      poll.start()
    }
  }

  Component.onCompleted: run("status")

  Timer {
    id: poll
    onTriggered: root.run("status")
  }

  Process {
    id: worker
    command: ["python3", "-B", decodeURIComponent(root.helper), root.action]
    onExited: root.finish(response.text)
    stdout: StdioCollector { id: response }
  }
}
