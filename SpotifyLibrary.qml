pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import qs.Commons
import qs.Ui

ColumnLayout {
  id: root

  required property var spotify
  required property var translations
  required property color foreground
  property string category: "tracks"
  property int offset: 0
  property bool pending: true
  readonly property bool loading: pending || (spotify.busy && spotify.action === "library")
  readonly property var page: spotify.library.kind === category && spotify.library.offset === offset
    ? spotify.library : {}
  readonly property var items: page.items ?? []
  readonly property int pageSize: 20

  spacing: Style.space(6)

  function load(kind, position) {
    root.category = kind
    root.offset = position
    root.pending = true
  }

  Timer {
    interval: 1
    running: root.pending && root.spotify.available && !root.spotify.busy
    onTriggered: {
      if (root.spotify.run("library", [root.category, String(root.offset)]))
        root.pending = false
    }
  }

  RowLayout {
    spacing: Style.space(4)
    Layout.fillWidth: true

    Repeater {
      model: ["tracks", "albums", "playlists"]
      Button {
        required property string modelData
        text: root.translations.t(modelData)
        foreground: root.foreground
        selected: root.category === modelData
        focusable: true
        Layout.fillWidth: true
        onClicked: if (root.category !== modelData) root.load(modelData, 0)
      }
    }
  }

  Text {
    visible: root.loading || root.items.length === 0
    textFormat: Text.PlainText
    font.family: Style.font.family
    font.pixelSize: Style.font.body
    color: root.foreground
    wrapMode: Text.WordWrap
    text: root.loading ? root.translations.t("libraryLoading")
      : root.translations.t(root.page.status && root.page.status !== "ok" ? root.page.status : "libraryEmpty")
    Layout.fillWidth: true
  }

  ListView {
    id: entries
    visible: !root.loading && root.items.length > 0
    clip: true
    reuseItems: true
    model: root.items
    Layout.fillWidth: true
    Layout.preferredHeight: Math.min(count, 7) * Style.space(34)

    Controls.ScrollBar.vertical: Controls.ScrollBar {}

    delegate: Button {
      required property var modelData
      text: modelData.subtitle ? modelData.title + " · " + modelData.subtitle : modelData.title
      foreground: root.foreground
      leftAlign: true
      focusable: true
      width: entries.width - Style.space(8)
      height: Style.space(34)
      Accessible.name: text
      enabled: !root.spotify.openingDesktop
      onClicked: root.spotify.openDesktop(modelData.uri)
    }
  }

  RowLayout {
    visible: !root.loading && (root.offset > 0 || Number.isInteger(root.page.next)
      || (!!root.page.status && root.page.status !== "ok"))
    spacing: Style.space(5)
    Layout.fillWidth: true

    Button {
      visible: root.offset > 0
      text: root.translations.t("previousPage")
      foreground: root.foreground
      focusable: true
      enabled: root.spotify.available
      onClicked: root.load(root.category, Math.max(0, root.offset - root.pageSize))
    }

    Button {
      visible: Number.isInteger(root.page.next)
      text: root.translations.t("nextPage")
      foreground: root.foreground
      focusable: true
      enabled: root.spotify.available
      onClicked: root.load(root.category, root.page.next)
    }

    Button {
      visible: !!root.page.status && root.page.status !== "ok"
      text: root.translations.t("retry")
      foreground: root.foreground
      focusable: true
      enabled: root.spotify.available
      onClicked: root.load(root.category, root.offset)
    }
  }
}
