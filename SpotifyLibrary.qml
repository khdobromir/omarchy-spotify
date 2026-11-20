pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import qs.Commons as Commons
import qs.Ui

ColumnLayout {
  id: root

  required property var spotify
  required property var translations
  required property color foreground
  required property bool browsing
  property string category: "tracks"
  property string displayedCategory: ""
  property var contexts: ({})
  property var request: null
  property bool restoring: false
  readonly property var context: contexts[category] ?? {}
  readonly property bool loading: context.status === "loading" || context.status === "loadingMore"
  readonly property bool failed: !!context.status && !loading && context.status !== "ok"

  spacing: Commons.Style.space(6)

  function update(kind, changes) {
    const state = Object.assign({ items: [], next: 0, scroll: 0, status: "loading", pending: true },
      root.contexts[kind], changes)
    const contexts = Object.assign({}, root.contexts)
    contexts[kind] = state
    root.contexts = contexts
  }

  function show(kind) {
    root.restoring = true
    if (!root.contexts[kind])
      root.update(kind, {})
    root.category = kind
    root.displayedCategory = kind
    rows.clear()
    for (const item of root.context.items)
      rows.append({ title: item.title, subtitle: item.subtitle, uri: item.uri })
    Qt.callLater(root.restoreScroll)
  }

  function restoreScroll() {
    entries.forceLayout()
    entries.contentY = entries.originY + Math.min(root.context.scroll ?? 0,
      Math.max(0, entries.contentHeight - entries.height))
    root.restoring = false
    root.maybeLoadMore()
  }

  function maybeLoadMore() {
    if (root.browsing && !root.restoring && root.context.status === "ok" && root.context.next >= 0
        && entries.contentHeight - (entries.contentY - entries.originY) - entries.height < Commons.Style.space(100))
      root.update(root.category, { status: "loadingMore", pending: true })
  }

  function applyPage(page) {
    if (!root.request || page.kind !== root.request.kind || page.offset !== root.request.offset)
      return
    const kind = root.request.kind
    root.request = null
    const state = root.contexts[kind]
    if (!state)
      return
    if (page.status !== "ok") {
      root.update(kind, { status: page.status })
      return
    }
    for (const item of page.items ?? []) {
      state.items.push(item)
      if (kind === root.displayedCategory)
        rows.append({ title: item.title, subtitle: item.subtitle, uri: item.uri })
    }
    root.update(kind, { status: "ok", next: Number.isInteger(page.next) && page.next > page.offset ? page.next : -1 })
    Qt.callLater(root.maybeLoadMore)
  }

  onBrowsingChanged: {
    if (browsing) {
      if (!displayedCategory)
        show(category)
      else
        Qt.callLater(root.maybeLoadMore)
    }
  }
  Component.onCompleted: if (root.browsing && !root.displayedCategory) root.show(root.category)

  Connections {
    target: root.spotify ?? null
    function onLibraryChanged() {
      if (root.spotify)
        root.applyPage(root.spotify.library)
    }
    function onLibraryInvalidated() {
      root.contexts = {}
      root.request = null
      root.displayedCategory = ""
      rows.clear()
      if (root.browsing)
        root.show(root.category)
    }
  }

  Timer {
    interval: 1
    running: root.browsing && !!root.context.pending && root.request === null
      && !!root.spotify?.available && !root.spotify.busy
    onTriggered: {
      const offset = root.context.next
      if (root.spotify?.run("library", [root.category, String(offset)])) {
        root.request = { kind: root.category, offset: offset }
        root.update(root.category, { pending: false })
      }
    }
  }

  ListModel { id: rows }

  RowLayout {
    spacing: Commons.Style.space(4)
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
        onClicked: if (root.category !== modelData) root.show(modelData)
      }
    }
  }

  Text {
    visible: entries.count === 0
    textFormat: Text.PlainText
    font.family: Commons.Style.font.family
    font.pixelSize: Commons.Style.font.body
    color: root.foreground
    wrapMode: Text.WordWrap
    text: root.loading ? root.translations.t("libraryLoading")
      : root.translations.t(root.failed ? root.context.status : "libraryEmpty")
    Layout.fillWidth: true
  }

  ListView {
    id: entries
    visible: count > 0
    clip: true
    reuseItems: true
    model: rows
    Layout.fillWidth: true
    Layout.preferredHeight: Math.min(count, 7) * Commons.Style.space(34)

    Controls.ScrollBar.vertical: Controls.ScrollBar {}

    onContentYChanged: {
      if (root.browsing && !root.restoring && root.contexts[root.displayedCategory])
        root.contexts[root.displayedCategory].scroll = Math.max(0, contentY - originY)
      Qt.callLater(root.maybeLoadMore)
    }
    onContentHeightChanged: Qt.callLater(root.maybeLoadMore)
    onHeightChanged: Qt.callLater(root.maybeLoadMore)

    delegate: Button {
      id: entry

      required property string title
      required property string subtitle
      required property string uri
      text: entry.subtitle ? entry.title + " · " + entry.subtitle : entry.title
      foreground: root.foreground
      leftAlign: true
      focusable: true
      width: entries.width - Commons.Style.space(8)
      height: Commons.Style.space(34)
      enabled: !!root.spotify && !root.spotify.openingDesktop
      Accessible.name: entry.text
      onClicked: root.spotify?.openDesktop(
        root.category === "tracks" ? "--play-track"
          : root.category === "albums" ? "--open-album" : "--open-playlist", entry.uri)
    }
  }

  RowLayout {
    visible: root.failed || (root.loading && entries.count > 0)
    spacing: Commons.Style.space(5)
    Layout.fillWidth: true

    Text {
      visible: entries.count > 0
      text: root.translations.t(root.loading ? "libraryLoadingMore" : "libraryMoreError")
      textFormat: Text.PlainText
      color: root.foreground
      font.family: Commons.Style.font.family
      font.pixelSize: Commons.Style.font.body
      elide: Text.ElideRight
      Layout.fillWidth: true
    }

    Button {
      visible: root.failed
      text: root.translations.t("retry")
      foreground: root.foreground
      focusable: true
      enabled: !!root.spotify?.available
      onClicked: root.update(root.category, { status: entries.count > 0 ? "loadingMore" : "loading", pending: true })
    }
  }
}
