pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import qs.Commons
import qs.Commons as Commons
import qs.Ui

PopupCard {
  id: root

  required property var spotify
  required property var sample
  required property string spotifyIcon
  readonly property bool hasTrack: sample.status === "track"
  readonly property bool hasProgress: hasTrack && sample.duration > 0 && sample.progress !== null
  readonly property bool signedIn: hasTrack || sample.status === "idle"
  readonly property bool extended: signedIn && sample.extended === true
  readonly property bool controls: extended && hasTrack && !!sample.deviceId
  readonly property bool ready: !!spotify?.available
  readonly property bool showingLibrary: extended && libraryOpen
  readonly property bool seekable: controls && hasProgress && allowed("seeking")
  readonly property var favorite: spotify?.favorite ?? {}
  readonly property bool favoriteKnown: favorite.id === sample.trackId && typeof favorite.saved === "boolean"
  readonly property bool connecting: !!spotify?.busy && spotify.action === "connect"
  readonly property bool canConnect: ["loading", "setup_required", "disconnected", "auth_error", "auth_timeout", "browser_error", "denied"].includes(sample.status)
  readonly property bool canRetry: sample.status === "forbidden" || sample.status === "unavailable"
  readonly property color textColor: Commons.Color.popups.text
  readonly property color secondaryColor: Qt.tint(textColor, Qt.alpha(Commons.Color.popups.background, 0.30))
  property bool libraryOpen: false
  property string favoriteRequested: ""
  property double now: 0
  readonly property real elapsed: hasProgress
    ? Math.min(sample.duration, sample.progress + (sample.playing ? Math.max(0, now - (spotify?.sampledAt ?? now)) : 0))
    : 0

  contentWidth: fittedContentWidth(Style.space(340))
  contentHeight: fittedContentHeight(body.implicitHeight)

  function timestamp(ms) {
    const seconds = Math.floor(ms / 1000)
    return Commons.I18n.formatNumber(Math.floor(seconds / 60)) + ":"
      + (seconds % 60 < 10 ? Commons.I18n.formatNumber(0) : "") + Commons.I18n.formatNumber(seconds % 60)
  }

  function allowed(action) {
    return !(sample.disallows ?? []).includes(action)
  }

  function seek(position) {
    if (root.ready && root.seekable)
      root.spotify.run("seek", [seekBar.dragDevice || root.sample.deviceId,
        seekBar.dragTrack || root.sample.trackId, String(Math.round(position))])
  }

  onOpenChanged: {
    if (open) {
      now = Date.now()
      favoriteRequested = ""
    } else {
      libraryOpen = false
    }
  }

  component PlainLabel: Text {
    textFormat: Text.PlainText
    font.family: Style.font.family
    color: root.textColor
  }

  ColumnLayout {
    id: body
    width: parent.width
    spacing: Style.space(10)

    Commons.Translations {
      id: strings
      directory: Qt.resolvedUrl("i18n")
    }

    Timer {
      interval: 500
      repeat: true
      running: root.open && !root.showingLibrary && root.hasProgress && root.sample.playing
      onTriggered: root.now = Date.now()
    }

    Timer {
      interval: 1
      running: root.open && !root.showingLibrary && root.hasTrack && root.extended
        && root.ready && !root.spotify.busy && root.favoriteRequested !== root.sample.trackId
      onTriggered: {
        if (root.spotify.run("favorite", [root.sample.trackId]))
          root.favoriteRequested = root.sample.trackId
      }
    }

    RowLayout {
      visible: root.extended
      spacing: Style.space(6)
      Layout.fillWidth: true

      Button {
        text: strings.t("nowPlaying")
        foreground: root.textColor
        selected: !root.showingLibrary
        focusable: true
        onClicked: root.libraryOpen = false
      }

      Button {
        text: strings.t("library")
        foreground: root.textColor
        selected: root.showingLibrary
        focusable: true
        onClicked: root.libraryOpen = true
      }
    }

    Loader {
      active: root.open && root.showingLibrary
      visible: active
      Layout.fillWidth: true
      sourceComponent: Component {
        SpotifyLibrary {
          spotify: root.spotify
          translations: strings
          foreground: root.textColor
        }
      }
    }

    RowLayout {
      Layout.fillWidth: true
      spacing: Style.space(10)
      visible: root.hasTrack && !root.showingLibrary

      SizedImage {
        visible: root.hasTrack && !!root.sample.art && status !== Image.Error
        url: root.open && root.hasTrack ? (root.sample.art ?? "") : ""
        Layout.preferredWidth: Style.space(72)
        Layout.preferredHeight: Style.space(72)
      }

      ColumnLayout {
        Layout.fillWidth: true
        spacing: Style.space(3)

        PlainLabel {
          Layout.fillWidth: true
          text: root.sample.title ?? ""
          font.pixelSize: Style.font.subtitle
          font.bold: true
          elide: Text.ElideRight
        }

        PlainLabel {
          Layout.fillWidth: true
          text: root.sample.artist ?? ""
          visible: text !== ""
          font.pixelSize: Style.font.body
          elide: Text.ElideRight
        }

        PlainLabel {
          Layout.fillWidth: true
          text: root.sample.album ?? ""
          visible: text !== ""
          color: root.secondaryColor
          font.pixelSize: Style.font.bodySmall
          elide: Text.ElideRight
        }
      }
    }

    Rectangle {
      Layout.fillWidth: true
      Layout.preferredHeight: Style.space(4)
      visible: root.hasProgress && !root.showingLibrary && !root.seekable
      radius: height / 2
      color: Qt.alpha(root.textColor, 0.22)
      Accessible.role: Accessible.ProgressBar
      Accessible.name: root.hasProgress ? strings.t("time").arg(root.timestamp(root.elapsed)).arg(root.timestamp(root.sample.duration)) : ""

      Rectangle {
        height: parent.height
        width: root.hasProgress ? parent.width * root.elapsed / root.sample.duration : 0
        anchors.left: parent.left
        radius: height / 2
        color: Commons.Color.accent
      }
    }

    PanelSlider {
      id: seekBar
      property string dragDevice: ""
      property string dragTrack: ""

      bar: root.bar
      visible: root.seekable && !root.showingLibrary
      enabled: root.ready || dragging
      value: root.elapsed
      maximum: root.sample.duration ?? 1
      step: 5000
      integer: true
      activeFocusOnTab: true
      Accessible.role: Accessible.Slider
      Accessible.name: strings.t("seek")
      Layout.fillWidth: true

      onDraggingChanged: if (dragging) {
        dragDevice = root.sample.deviceId
        dragTrack = root.sample.trackId
      }
      onVisibleChanged: if (!visible) {
        dragging = false
        dragDevice = ""
        dragTrack = ""
      }
      onReleased: function(position) {
        root.seek(position)
        dragDevice = ""
        dragTrack = ""
      }
      Keys.onLeftPressed: root.seek(Math.max(0, value - step))
      Keys.onRightPressed: root.seek(Math.min(maximum, value + step))
    }

    PlainLabel {
      Layout.fillWidth: true
      visible: root.hasTrack && !root.showingLibrary
      font.pixelSize: Style.font.bodySmall
      color: root.secondaryColor
      elide: Text.ElideRight
      text: {
        const duration = root.sample.duration
        const timeLabel = root.hasProgress
          ? strings.t("time").arg(root.timestamp(seekBar.dragging ? seekBar.liveValue : root.elapsed)).arg(root.timestamp(duration))
          : (duration > 0 ? root.timestamp(duration) : "")
        const deviceLabel = root.sample.device ?? ""
        return [timeLabel, root.sample.playing ? deviceLabel
          : (deviceLabel ? strings.t("pausedOn").arg(deviceLabel) : strings.t("paused"))]
          .filter(Boolean).join("  ·  ")
      }
    }

    RowLayout {
      visible: root.extended && root.hasTrack && !root.showingLibrary
      spacing: Style.space(5)

      Button {
        iconText: "󰒮"
        tooltipText: strings.t("previous")
        foreground: root.textColor
        focusable: true
        Accessible.role: Accessible.Button
        Accessible.name: tooltipText
        enabled: root.ready && root.controls && root.allowed("skipping_prev")
        onClicked: root.spotify.run("previous", [root.sample.deviceId])
      }

      Button {
        iconText: root.sample.playing ? "󰏤" : "󰐊"
        tooltipText: strings.t(root.sample.playing ? "pause" : "play")
        foreground: root.textColor
        focusable: true
        Accessible.role: Accessible.Button
        Accessible.name: tooltipText
        enabled: root.ready && root.controls && root.allowed(root.sample.playing ? "pausing" : "resuming")
        onClicked: root.spotify.run(root.sample.playing ? "pause" : "play", [root.sample.deviceId])
      }

      Button {
        iconText: "󰒭"
        tooltipText: strings.t("next")
        foreground: root.textColor
        focusable: true
        Accessible.role: Accessible.Button
        Accessible.name: tooltipText
        enabled: root.ready && root.controls && root.allowed("skipping_next")
        onClicked: root.spotify.run("next", [root.sample.deviceId])
      }

      Button {
        iconText: root.favoriteKnown && root.favorite.saved ? "󰄬" : "󰐕"
        tooltipText: strings.t(root.favoriteKnown && root.favorite.saved ? "removeFavorite" : "saveFavorite")
        foreground: root.textColor
        focusable: true
        Accessible.role: Accessible.Button
        Accessible.name: tooltipText
        enabled: root.ready && root.favoriteKnown
        onClicked: root.spotify.run(root.favorite.saved ? "remove" : "save", [root.sample.trackId])
      }
    }

    PlainLabel {
      Layout.fillWidth: true
      visible: !root.hasTrack && !root.showingLibrary
      font.pixelSize: Style.font.body
      wrapMode: Text.WordWrap
      text: root.connecting ? strings.t("connecting") : strings.t(root.sample.status)
    }

    PlainLabel {
      readonly property string notice: root.spotify?.notice ?? ""
      visible: (root.connecting && root.hasTrack) || (!!notice && notice !== root.sample.status)
      font.pixelSize: Style.font.bodySmall
      wrapMode: Text.WordWrap
      text: root.connecting ? strings.t("connecting") : (notice ? strings.t(notice) : "")
      Layout.fillWidth: true
    }

    Button {
      visible: root.signedIn && !root.extended
      enabled: root.ready && !root.connecting
      text: strings.t("grantAccess")
      foreground: root.textColor
      focusable: true
      onClicked: root.spotify.run("connect")
    }

    RowLayout {
      Layout.fillWidth: true
      spacing: Style.space(6)
      visible: !root.showingLibrary

      Button {
        iconText: root.spotifyIcon
        text: root.hasTrack ? strings.t("openSpotify") : strings.t("connect")
        foreground: root.textColor
        focusable: true
        visible: root.hasTrack || root.canConnect
        enabled: root.hasTrack || root.ready
        onClicked: {
          if (root.hasTrack)
            Qt.openUrlExternally(root.sample.url)
          else
            root.spotify.run("connect")
        }
      }

      Button {
        text: strings.t("retry")
        foreground: root.textColor
        focusable: true
        visible: root.canRetry
        enabled: root.ready
        onClicked: root.spotify.run("status")
      }

      Item { Layout.fillWidth: true }

      Button {
        text: strings.t("disconnect")
        foreground: root.secondaryColor
        focusable: true
        visible: !root.canConnect
        enabled: !!root.spotify && (!root.spotify.busy || root.ready)
        onClicked: root.spotify.run("disconnect")
      }
    }
  }
}
