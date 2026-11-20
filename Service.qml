pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io

Scope {
  id: root

  property var sample: ({ status: "loading" })
  property var favorite: ({})
  property var library: ({})
  property double sampledAt: 0
  property double revision: -1
  property double accountRevision: -1
  property int sequence: 0
  property string requestId: ""
  property string action: ""
  property var args: []
  property string notice: ""
  property string desktopAction: "--activate"
  property string desktopUri: ""
  property string desktopNotice: ""
  property bool subscribed: false
  property bool triedLaunch: false
  readonly property bool openingDesktop: desktop.running
  readonly property bool busy: requestId !== ""
  readonly property bool coolingDown: ["limited", "quota"].includes(sample.sync)
  readonly property bool available: subscribed && !busy && !coolingDown
  signal libraryInvalidated()

  Component.onCompleted: reconnect()

  function openDesktop(action, uri) {
    if (desktop.running)
      return
    root.desktopNotice = ""
    root.desktopAction = action
    root.desktopUri = uri ?? ""
    desktop.running = true
  }

  function send(id, method, params) {
    channel.write(JSON.stringify({ version: 1, id, method, params }) + "\n")
    channel.flush()
  }

  function run(command, parameters) {
    if (!subscribed || busy || (coolingDown && !["disconnect", "status"].includes(command)))
      return false
    const args = parameters ?? []
    let params = { args }
    let method = command
    if (["next", "previous", "play", "pause", "seek"].includes(command)) {
      method = "playback." + (command === "play" ? "resume" : command)
      params = { deviceId: args[0] }
      if (command === "seek") {
        params.uri = args[1]
        params.position = Number(args[2])
      }
    } else if (command === "status") {
      method = "refresh"
      params = {}
    }
    root.action = command
    root.args = args
    root.notice = ""
    root.requestId = String(++root.sequence)
    deadline.interval = command === "connect" ? 195000 : 20000
    deadline.restart()
    root.send(root.requestId, method, params)
    return true
  }

  function applySample(data) {
    if (data.revision <= revision)
      return
    root.revision = data.revision
    if (data.accountRevision !== accountRevision) {
      root.accountRevision = data.accountRevision
      root.favorite = {}
      root.libraryInvalidated()
    }
    const same = sample.trackId === data.trackId
    root.sampledAt = Date.now() - data.sampleAgeMs
    root.sample = data
    if (!same)
      root.favorite = {}
  }

  function finish(result) {
    const command = root.action
    const args = root.args
    root.requestId = ""
    deadline.stop()
    const status = result.status
    if (command === "library")
      root.library = Object.assign(result, { kind: args[0], offset: Number(args[1]) })
    if (["favorite", "save", "remove"].includes(command) && status === "ok")
      root.favorite = Object.assign(result, { id: args[0] })
    if (!result.accepted && !["ok", "authorized", "disconnected"].includes(status))
      root.notice = status || "action_failed"
  }

  function receive(line) {
    try {
      const message = JSON.parse(line)
      if (message.version !== 1 || line.length > 262144)
        throw new Error("Invalid Core message")
      if (message.event === "playback.changed")
        root.applySample(message.data)
      else if (message.event === "library.changed") {
        const change = message.data
        if (change.trackId === root.sample.trackId)
          root.favorite = { id: change.trackId, saved: change.saved }
        root.libraryInvalidated()
      } else if (message.id === "subscribe" && message.ok) {
        handshake.stop()
        root.subscribed = true
        root.applySample(message.result)
      } else if (message.id === root.requestId) {
        root.finish(message.ok ? message.result : { status: message.error.code })
      }
    } catch (error) {
      channel.connected = false
    }
  }

  function reconnect() {
    if (!channel.connected && channel.path !== "") {
      channel.connected = true
      handshake.restart()
    }
  }

  Socket {
    id: channel
    path: {
      const runtime = Quickshell.env("XDG_RUNTIME_DIR")
      return runtime ? runtime + "/omarchy-spotify/core.sock" : ""
    }
    parser: SplitParser {
      splitMarker: "\n"
      onRead: line => root.receive(line)
    }
    onConnectionStateChanged: {
      if (connected) {
        root.triedLaunch = false
        root.revision = -1
        root.accountRevision = -1
        root.send("subscribe", "subscribe", {})
      } else {
        root.subscribed = false
        if (root.busy)
          root.finish({ status: "unavailable" })
        root.sample = { status: "core_unavailable" }
        root.favorite = {}
        retry.restart()
      }
    }
    onError: {
      if (!root.triedLaunch) {
        root.triedLaunch = true
        starter.running = true
      }
      retry.restart()
    }
  }

  Timer { id: retry; interval: 1000; onTriggered: root.reconnect() }
  Timer {
    id: handshake
    interval: 5000
    onTriggered: {
      if (!root.subscribed) {
        channel.connected = false
        retry.restart()
      }
    }
  }
  Timer { id: deadline; onTriggered: root.finish({ status: "unavailable" }) }
  Process {
    id: starter
    command: ["omarchy-spotify-core", "--activate"]
    // On-demand fallback is detached by the installed launcher.
  }
  Process {
    id: desktop
    command: ["omarchy-spotify-open", root.desktopAction].concat(root.desktopUri ? [root.desktopUri] : [])
    onExited: {
      let status = "desktop_failed"
      try {
        status = JSON.parse(launchResult.text).status
      } catch (error) {}
      root.desktopNotice = status === "ok" ? "" : status
    }
    stdout: StdioCollector { id: launchResult }
  }
}
