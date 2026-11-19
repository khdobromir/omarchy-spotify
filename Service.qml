import QtQuick
import Quickshell
import Quickshell.Io

Scope {
  id: root

  property var sample: ({ status: "loading" })
  property var favorite: ({})
  property var library: ({})
  property double sampledAt: 0
  property string action: "status"
  property var args: []
  property string notice: ""
  property var queued: null
  property string desktopAction: "--activate"
  property string desktopUri: ""
  property string desktopNotice: ""
  readonly property bool openingDesktop: desktop.running
  property bool coolingDown: false
  property bool statusDue: false
  readonly property bool busy: worker.running
  readonly property bool available: !coolingDown && !statusDue && queued === null && (!busy || action === "status")
  readonly property string helper: Qt.resolvedUrl("spotify.py").toString().slice("file://".length)

  function openDesktop(action, uri) {
    if (desktop.running)
      return
    root.desktopNotice = ""
    root.desktopAction = action
    root.desktopUri = uri ?? ""
    desktop.running = true
  }

  function run(command, parameters) {
    if (root.coolingDown && command !== "disconnect")
      return false
    if (root.busy) {
      if (root.action !== "status" || root.queued !== null || command === "status")
        return false
      root.queued = [command, parameters]
      return true
    }
    if (command !== "library")
      poll.stop()
    if (command === "status")
      root.statusDue = false
    root.action = command
    root.args = parameters ?? []
    if (!["status", "favorite", "library"].includes(command))
      root.notice = ""
    worker.running = true
    return true
  }

  function finish(text) {
    let result
    try {
      result = JSON.parse(text)
    } catch (error) {
      const fallback = { status: "unavailable", connect: "auth_error", disconnect: "disconnect_error" }
      result = { status: fallback[root.action] ?? "action_failed" }
      if (root.action === "status")
        result.retry = 30
    }
    const status = result.status
    const command = root.action
    const sessionEnded = status === "disconnected" || status === "setup_required"
    const playbackChanged = ["next", "previous", "pause", "play", "seek"].includes(command)
    if (status === "limited" || status === "quota") {
      root.coolingDown = true
      root.queued = null
    }
    if (command === "status" || sessionEnded || status === "authorized") {
      root.sampledAt = Date.now()
      root.sample = result
      if (command === "status" && (root.notice === "limited" || root.notice === "quota"))
        root.notice = ""
    }
    if (sessionEnded || status === "authorized") {
      root.favorite = {}
      root.library = {}
      if (sessionEnded)
        root.coolingDown = false
    }
    if (command === "library") {
      root.library = Object.assign(result, { kind: root.args[0], offset: Number(root.args[1]) })
    } else if (["favorite", "save", "remove"].includes(command) && !sessionEnded) {
      if (status === "ok" || command === "favorite")
        root.favorite = Object.assign(result, { id: root.args[0] })
    }
    if (root.queued && sessionEnded && !["connect", "disconnect"].includes(root.queued[0]))
      root.queued = null
    if (root.queued && ["next", "previous", "pause", "play", "seek"].includes(root.queued[0])
        && root.queued[1][0] !== result.deviceId) {
      root.queued = null
      root.notice = "target_changed"
    }
    if (status === "reauthorize")
      root.sample = Object.assign({}, root.sample, { extended: false })
    if (command !== "status" && command !== "library" && status !== "ok" && status !== "authorized" && !sessionEnded) {
      if (command === "connect" && root.sample.status !== "track" && root.sample.status !== "idle")
        root.sample = result
      else
        root.notice = status
    }
    let delay = 0
    if (result.retry)
      delay = result.retry * 1000
    else if (root.queued !== null || status === "authorized" || root.statusDue)
      delay = 1
    else if (playbackChanged)
      delay = 600
    else if (root.sample.status === "track" || root.sample.status === "idle")
      delay = root.sample.playing ? 10000 : 25000
    if (delay > 0 && (!poll.running || result.retry)) {
      poll.interval = Math.min(delay, 2147483647)
      poll.start()
    }
  }

  Component.onCompleted: run("status")

  Timer {
    id: poll
    onTriggered: {
      if (root.busy) {
        root.statusDue = true
        return
      }
      root.coolingDown = false
      const pending = root.queued
      root.queued = null
      if (pending)
        root.run(pending[0], pending[1])
      else
        root.run("status")
    }
  }

  Process {
    id: desktop
    command: ["python3", "-B", decodeURIComponent(root.helper), "desktop", root.desktopAction]
      .concat(root.desktopUri ? [root.desktopUri] : [])
    onExited: {
      let status = "desktop_failed"
      try {
        status = JSON.parse(launchResult.text).status
      } catch (error) {}
      root.desktopNotice = status === "ok" ? "" : status
    }
    stdout: StdioCollector { id: launchResult }
  }

  Process {
    id: worker
    command: ["python3", "-B", decodeURIComponent(root.helper), root.action].concat(root.args)
    onExited: root.finish(response.text)
    stdout: StdioCollector { id: response }
  }
}
