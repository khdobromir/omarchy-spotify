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
  readonly property bool connecting: !!spotify?.busy && spotify.action === "connect"
  readonly property bool canConnect: ["loading", "setup_required", "disconnected", "auth_error", "auth_timeout", "browser_error", "denied"].includes(sample.status)
  readonly property bool canRetry: sample.status === "forbidden" || sample.status === "unavailable"
  readonly property color textColor: Commons.Color.popups.text
  readonly property color secondaryColor: Qt.tint(textColor, Qt.alpha(Commons.Color.popups.background, 0.30))
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

  onOpenChanged: if (open) now = Date.now()

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
      running: root.open && root.hasProgress && root.sample.playing
      onTriggered: root.now = Date.now()
    }

    RowLayout {
      Layout.fillWidth: true
      spacing: Style.space(10)
      visible: root.hasTrack

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
      visible: root.hasProgress
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

    PlainLabel {
      Layout.fillWidth: true
      visible: root.hasTrack
      font.pixelSize: Style.font.bodySmall
      color: root.secondaryColor
      elide: Text.ElideRight
      text: {
        const duration = root.sample.duration
        const timeLabel = root.hasProgress
          ? strings.t("time").arg(root.timestamp(root.elapsed)).arg(root.timestamp(duration))
          : (duration > 0 ? root.timestamp(duration) : "")
        const deviceLabel = root.sample.device ?? ""
        return [timeLabel, root.sample.playing ? deviceLabel
          : (deviceLabel ? strings.t("pausedOn").arg(deviceLabel) : strings.t("paused"))]
          .filter(Boolean).join("  ·  ")
      }
    }

    PlainLabel {
      Layout.fillWidth: true
      visible: !root.hasTrack
      font.pixelSize: Style.font.body
      wrapMode: Text.WordWrap
      text: root.connecting ? strings.t("connecting") : strings.t(root.sample.status)
    }

    RowLayout {
      Layout.fillWidth: true
      spacing: Style.space(6)

      Button {
        iconText: root.spotifyIcon
        text: root.hasTrack ? strings.t("openSpotify") : strings.t("connect")
        foreground: root.textColor
        focusable: true
        visible: root.hasTrack || root.canConnect
        enabled: root.hasTrack || (!!root.spotify && !root.spotify.busy)
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
        enabled: !!root.spotify && !root.spotify.busy
        onClicked: root.spotify.run("status")
      }

      Item { Layout.fillWidth: true }

      Button {
        text: strings.t("disconnect")
        foreground: root.secondaryColor
        focusable: true
        visible: !root.canConnect
        enabled: !!root.spotify && !root.spotify.busy
        onClicked: root.spotify.run("disconnect")
      }
    }
  }
}
