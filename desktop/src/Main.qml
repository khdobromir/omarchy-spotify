pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmarchySpotify

ApplicationWindow {
    id: window

    required property Backend backend
    required property Theme theme
    required property url preferences

    readonly property var track: backend.playback
    readonly property bool canPlay: track.ready && !backend.commandBusy
    readonly property bool keyEntry: ["key_required", "key_write", "player_failed"].includes(backend.playerStatus)
    property bool nowPlayingExpanded: false
    readonly property string trackTitle: {
        if (track.title)
            return track.title
        if (!track.ready)
            return qsTr("Player disconnected")
        if (track.idle)
            return qsTr("Nothing is playing")
        if (track.loadingMetadata || !track.uri)
            return qsTr("Loading track…")
        return qsTr("Track details unavailable")
    }
    readonly property bool showingCollection: backend.collectionUri !== ""
    readonly property bool showingGrid: backend.category === "albums" && !showingCollection
    readonly property bool showingLists: backend.category === "playlists" && !showingCollection
    readonly property bool canConnect: ["disconnected", "reauthorize", "denied", "auth_error", "auth_timeout", "browser_error"]
        .includes(backend.libraryStatus)
    readonly property color muted: Qt.tint(theme.foreground, Qt.alpha(theme.background, 0.35))
    readonly property color surface: Qt.tint(theme.background, Qt.alpha(theme.foreground, theme.light ? 0.045 : 0.065))
    readonly property color line: Qt.alpha(theme.foreground, 0.13)
    readonly property int inset: width < 620 ? 12 : 22
    property string restoredContext: ""
    property bool restorePending: true

    title: "Omarchy Spotify"
    color: theme.background
    visible: false

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    Settings {
        id: prefs
        location: window.preferences
        category: "Library"
        property bool compact: true
    }

    function switchCategory(kind: string) {
        if (backend.category !== kind || showingCollection)
            backend.loadLibrary(kind)
    }

    onNowPlayingExpandedChanged: {
        if (!nowPlayingExpanded) {
            Qt.callLater(window.restoreScroll)
            Qt.callLater(() => (window.showingGrid ? albums : rows).forceActiveFocus())
        }
        Qt.callLater(window.maybeLoadMore)
    }

    Connections {
        target: window.backend
        function onLibraryContextChanged() {
            window.scheduleRestore()
        }
        function onLibraryChanged() {
            Qt.callLater(window.maybeLoadMore)
        }
    }

    function columnCount(view: Flickable): int {
        return view === albums ? Math.max(1, Math.floor((albums.width - 9) / albums.cellWidth)) : 1
    }

    function rememberScroll(view: Flickable) {
        if (!view.visible || restorePending || restoredContext !== backend.libraryContext)
            return
        const stride = view === albums ? albums.cellHeight : 55
        backend.rememberScroll(Math.max(0, view.contentY - view.originY) * columnCount(view) / stride)
    }

    function scheduleRestore() {
        restorePending = true
        Qt.callLater(window.restoreScroll)
    }

    function restoreScroll() {
        if (!restorePending || nowPlayingExpanded)
            return
        if (showingGrid) {
            if (albums.count === 0)
                return
            albums.forceLayout()
            albums.contentY = albums.originY + Math.min(backend.libraryScroll() / columnCount(albums) * albums.cellHeight,
                Math.max(0, albums.contentHeight - albums.height))
        } else {
            if (rows.count === 0)
                return
            rows.forceLayout()
            rows.contentY = rows.originY + Math.min(backend.libraryScroll() * 55,
                Math.max(0, rows.contentHeight - rows.height))
        }
        restoredContext = backend.libraryContext
        restorePending = false
        maybeLoadMore()
    }

    function maybeLoadMore() {
        if (nowPlayingExpanded || backend.libraryStatus !== "ok" || backend.nextOffset < 0)
            return
        const view = showingGrid ? albums : rows
        if (view.height > 0 && view.contentHeight - (view.contentY - view.originY) - view.height < 220)
            backend.loadMore()
    }

    function activateEntry(entry: var) {
        if (showingGrid || showingLists)
            backend.openCollection(entry.uri, entry)
        else
            backend.play(entry.uri)
    }

    function timeText(ms: int): string {
        const seconds = Math.floor(ms / 1000)
        return Math.floor(seconds / 60).toString() + ":" + (seconds % 60).toString().padStart(2, "0")
    }

    function playerMessage(status: string): string {
        switch (status) {
        case "starting": return qsTr("Starting Spotify Soloist…")
        case "key_required": return qsTr("Add your personal Soloist API key.")
        case "key_write": return qsTr("Could not save the key on this computer.")
        case "pairing": return qsTr("In Spotify on your phone, select Omarchy Spotify from the device list.")
        case "player_missing": return qsTr("Spotify Soloist is missing. Install it and retry.")
        case "player_expired": return qsTr("This Spotify Soloist build expired. Install a newer build and retry.")
        case "player_connection":
        case "player_timeout": return qsTr("Could not connect to the local player.")
        case "player_failed": return qsTr("Soloist stopped. Check the key and try again.")
        case "player_command": return qsTr("Soloist could not perform that action.")
        default: return ""
        }
    }

    function libraryMessage(status: string): string {
        switch (status) {
        case "loading":
        case "loadingMore": return qsTr("Loading your library…")
        case "connecting": return qsTr("Approve library access in your browser.")
        case "setup_required": return qsTr("Save your Spotify Client ID in the Omarchy configuration first.")
        case "disconnected":
        case "reauthorize": return qsTr("Connect Spotify to browse your library.")
        case "quota":
        case "limited": return qsTr("Spotify paused requests. Try again later.")
        case "collection_forbidden": return qsTr("Spotify only shares tracks from playlists you own or collaborate on.")
        case "forbidden": return qsTr("Spotify denied access to this content.")
        case "ok": return qsTr("This library is empty.")
        default: return qsTr("Could not load your Spotify library.")
        }
    }

    Shortcut {
        sequence: "Space"
        enabled: window.canPlay && !keyField.activeFocus
        onActivated: window.backend.togglePlayback()
    }
    Shortcut { sequence: "Ctrl+1"; enabled: !window.nowPlayingExpanded; onActivated: window.switchCategory("tracks") }
    Shortcut { sequence: "Ctrl+2"; enabled: !window.nowPlayingExpanded; onActivated: window.switchCategory("albums") }
    Shortcut { sequence: "Ctrl+3"; enabled: !window.nowPlayingExpanded; onActivated: window.switchCategory("playlists") }
    Shortcut {
        sequence: "Ctrl+G"
        enabled: window.showingGrid && !window.nowPlayingExpanded
        onActivated: prefs.compact = !prefs.compact
    }
    Shortcut { sequence: "Alt+Left"; enabled: window.showingCollection && !window.nowPlayingExpanded; onActivated: window.backend.back() }
    Shortcut { sequence: "F11"; onActivated: window.nowPlayingExpanded = !window.nowPlayingExpanded }
    Shortcut { sequence: "Escape"; enabled: window.nowPlayingExpanded; onActivated: window.nowPlayingExpanded = false }
    Shortcut { sequences: [StandardKey.Quit]; onActivated: window.close() }

    component NavButton: AbstractButton {
        id: tab

        required property string kind
        readonly property bool selected: kind === window.backend.category
        implicitWidth: Math.max(78, tabCaption.implicitWidth + 28)
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.PageTab
        Accessible.selected: selected
        onClicked: window.switchCategory(kind)

        contentItem: Label {
            id: tabCaption
            text: tab.text
            color: tab.selected ? window.theme.accent : window.muted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 13
            font.weight: tab.selected ? Font.DemiBold : Font.Medium
        }
        background: Rectangle {
            radius: 7
            color: tab.selected ? Qt.alpha(window.theme.accent, 0.13)
                                : tab.hovered ? window.surface : "transparent"
            border.width: tab.visualFocus ? 1 : 0
            border.color: window.theme.accent
        }
    }

    component RowButton: MediaButton {
        foreground: window.theme.foreground
        emphasis: window.theme.accent
        emphasisText: window.theme.accentForeground
    }

    component PageLabel: Label {
        color: window.muted
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.pixelSize: 12
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !window.nowPlayingExpanded

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: window.theme.background

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: window.inset
                anchors.rightMargin: window.inset
                spacing: 5

                NavButton { kind: "tracks"; text: qsTr("Liked songs") }
                NavButton { kind: "albums"; text: qsTr("Albums") }
                NavButton { kind: "playlists"; text: qsTr("Playlists") }
                Item { Layout.fillWidth: true }
                RowLayout {
                    visible: window.showingGrid
                    spacing: 3
                    RowButton {
                        glyph: "compact"
                        text: qsTr("Compact grid")
                        checkable: true
                        autoExclusive: true
                        checked: prefs.compact
                        primary: checked
                        onClicked: prefs.compact = true
                    }
                    RowButton {
                        glyph: "grid"
                        text: qsTr("Default grid")
                        checkable: true
                        autoExclusive: true
                        checked: !prefs.compact
                        primary: checked
                        onClicked: prefs.compact = false
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: window.line
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: window.inset
            Layout.rightMargin: window.inset
            Layout.topMargin: 12
            Layout.bottomMargin: 8
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: 5
                visible: window.showingCollection
                spacing: 12

                RowButton {
                    Layout.alignment: Qt.AlignTop
                    glyph: "back"
                    text: qsTr("Back")
                    onClicked: window.backend.back()
                }
                Artwork {
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 64
                    glyph: window.backend.category === "albums" ? "album" : "playlist"
                    source: window.backend.collection.art || ""
                    decodeSize: 192
                    ink: window.theme.foreground
                    surround: window.theme.background
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: window.backend.collection.title || (window.backend.category === "albums" ? qsTr("Album") : qsTr("Playlist"))
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                        Accessible.role: Accessible.Heading
                    }
                    PageLabel {
                        Layout.fillWidth: true
                        text: window.backend.collection.subtitle || ""
                        visible: text !== ""
                    }
                }
                RowButton {
                    diameter: 40
                    glyph: "play"
                    text: qsTr("Play collection")
                    primary: true
                    enabled: window.backend.collection.title !== undefined
                    onClicked: window.backend.play(window.backend.collectionUri)
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: rows
                    anchors.fill: parent
                    visible: !window.showingGrid
                    focus: visible
                    clip: true
                    reuseItems: true
                    model: !window.showingGrid ? window.backend.entries : null
                    cacheBuffer: 66
                    ScrollBar.vertical: ScrollBar {}
                    onContentYChanged: {
                        window.rememberScroll(rows)
                        Qt.callLater(window.maybeLoadMore)
                    }
                    onCountChanged: Qt.callLater(window.restoreScroll)
                    onHeightChanged: Qt.callLater(window.maybeLoadMore)
                    onContentHeightChanged: Qt.callLater(window.maybeLoadMore)
                    Keys.onReturnPressed: if (currentIndex >= 0 && currentIndex < count) window.activateEntry(window.backend.entryAt(currentIndex))
                    Keys.onEnterPressed: if (currentIndex >= 0 && currentIndex < count) window.activateEntry(window.backend.entryAt(currentIndex))

                    delegate: ItemDelegate {
                        id: entry

                        required property int index
                        required property var entryData
                        readonly property bool current: window.track.uri === entryData.uri || window.backend.selectedUri === entryData.uri
                        readonly property color rowColor: current ? Qt.tint(window.theme.background, Qt.alpha(window.theme.accent, 0.12))
                            : hovered || ListView.isCurrentItem && rows.activeFocus ? window.surface : window.theme.background

                        width: rows.width - 9
                        height: 55
                        padding: 6
                        hoverEnabled: true
                        Accessible.name: [entryData.title, entryData.subtitle].filter(Boolean).join(", ")
                        onClicked: {
                            window.rememberScroll(rows)
                            rows.currentIndex = index
                            rows.forceActiveFocus()
                            window.activateEntry(entryData)
                        }

                        background: Rectangle {
                            radius: 7
                            color: entry.rowColor
                            border.width: entry.ListView.isCurrentItem && rows.activeFocus ? 1 : 0
                            border.color: window.line

                            Rectangle {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 2
                                height: parent.height - 20
                                radius: 1
                                visible: entry.current
                                color: window.theme.accent
                            }
                        }

                        contentItem: RowLayout {
                            spacing: 10
                            Artwork {
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                glyph: window.showingLists ? "playlist" : "note"
                                source: entry.entryData.art || ""
                                ink: window.theme.foreground
                                surround: entry.rowColor
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: entry.entryData.title || ""
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: entry.current ? window.theme.accent : window.theme.foreground
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                }
                                PageLabel {
                                    Layout.fillWidth: true
                                    text: entry.entryData.subtitle || ""
                                    visible: text !== ""
                                }
                            }
                            PageLabel {
                                Layout.rightMargin: 6
                                visible: !!entry.entryData.duration
                                text: window.timeText(entry.entryData.duration || 0)
                            }
                            MediaIcon {
                                Layout.rightMargin: 6
                                visible: window.showingLists
                                name: "forward"
                                color: window.muted
                            }
                        }
                    }
                }

                GridView {
                    id: albums
                    anchors.fill: parent
                    visible: window.showingGrid
                    focus: visible
                    clip: true
                    reuseItems: true
                    model: window.showingGrid ? window.backend.entries : null
                    cacheBuffer: 60
                    cellWidth: Math.floor((width - 9) / Math.max(1, Math.floor((width - 9) / (prefs.compact ? 105 : 132))))
                    cellHeight: cellWidth + (prefs.compact ? 0 : 43)
                    ScrollBar.vertical: ScrollBar {}
                    onContentYChanged: {
                        window.rememberScroll(albums)
                        Qt.callLater(window.maybeLoadMore)
                    }
                    onCellHeightChanged: if (visible && count > 0) window.scheduleRestore()
                    onCountChanged: Qt.callLater(window.restoreScroll)
                    onHeightChanged: Qt.callLater(window.maybeLoadMore)
                    onContentHeightChanged: Qt.callLater(window.maybeLoadMore)
                    Keys.onReturnPressed: if (currentIndex >= 0 && currentIndex < count) window.activateEntry(window.backend.entryAt(currentIndex))
                    Keys.onEnterPressed: if (currentIndex >= 0 && currentIndex < count) window.activateEntry(window.backend.entryAt(currentIndex))

                    delegate: ItemDelegate {
                        id: tile

                        required property int index
                        required property var entryData
                        readonly property bool selected: GridView.isCurrentItem && albums.activeFocus
                        readonly property color tileColor: hovered || selected ? window.surface : window.theme.background

                        width: albums.cellWidth - 4
                        height: albums.cellHeight - 4
                        padding: 4
                        hoverEnabled: true
                        Accessible.name: [entryData.title, entryData.subtitle].filter(Boolean).join(", ")
                        ToolTip.visible: hovered && prefs.compact
                        ToolTip.delay: 450
                        ToolTip.text: Accessible.name
                        onClicked: {
                            window.rememberScroll(albums)
                            albums.currentIndex = index
                            albums.forceActiveFocus()
                            window.activateEntry(entryData)
                        }

                        background: Rectangle {
                            color: tile.tileColor
                            radius: 9
                            border.width: tile.selected || tile.down ? 1 : 0
                            border.color: window.theme.accent
                        }

                        contentItem: ColumnLayout {
                            spacing: prefs.compact ? 0 : 3
                            Artwork {
                                Layout.fillWidth: true
                                Layout.preferredHeight: albums.cellWidth - 12
                                glyph: "album"
                                source: (width * Screen.devicePixelRatio > 260 ? tile.entryData.artHigh : tile.entryData.art) || ""
                                decodeSize: width * Screen.devicePixelRatio > 260 ? 512 : 256
                                ink: window.theme.foreground
                                surround: tile.tileColor
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.topMargin: 3
                                visible: !prefs.compact
                                text: tile.entryData.title || ""
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            PageLabel {
                                Layout.fillWidth: true
                                visible: !prefs.compact
                                text: tile.entryData.subtitle || ""
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 24, 410)
                    visible: (window.showingGrid ? albums.count : rows.count) === 0
                    spacing: 12

                    PageLabel {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        font.pixelSize: 14
                        text: window.libraryMessage(window.backend.libraryStatus)
                    }
                    Button {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Connect Spotify")
                        visible: window.canConnect
                        enabled: !window.backend.webBusy && !window.backend.webCoolingDown
                        onClicked: window.backend.connectLibrary()
                    }
                    Button {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Retry")
                        visible: ["unavailable", "forbidden", "action_failed", "quota", "limited"].includes(window.backend.libraryStatus)
                        enabled: !window.backend.webBusy && !window.backend.webCoolingDown
                        onClicked: window.backend.retryLibrary()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: (window.showingGrid ? albums.count : rows.count) > 0
                    && window.backend.libraryStatus !== "ok"
                spacing: 8

                PageLabel {
                    Layout.fillWidth: true
                    text: window.backend.libraryStatus === "loadingMore" ? qsTr("Loading more…")
                        : window.backend.libraryStatus === "connecting" ? window.libraryMessage("connecting")
                        : qsTr("Could not load more.") + " " + window.libraryMessage(window.backend.libraryStatus)
                }
                Button {
                    text: window.canConnect ? qsTr("Connect Spotify") : qsTr("Retry")
                    visible: !["loadingMore", "connecting"].includes(window.backend.libraryStatus)
                    enabled: !window.backend.webBusy && !window.backend.webCoolingDown
                    onClicked: window.canConnect ? window.backend.connectLibrary() : window.backend.retryLibrary()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: notice.implicitHeight + 14
            visible: window.backend.playerStatus !== "ready" || window.backend.selectionStatus !== ""
            color: window.surface

            ColumnLayout {
                id: notice
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: window.inset
                anchors.rightMargin: window.inset
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true

                    Artwork {
                        visible: window.backend.selectionStatus === "inspecting"
                        Layout.preferredWidth: 42
                        Layout.preferredHeight: 42
                        source: window.backend.selection.art || ""
                        ink: window.theme.foreground
                        surround: window.surface
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        PageLabel {
                            Layout.fillWidth: true
                            visible: window.backend.selectionStatus !== ""
                            color: window.theme.accent
                            text: window.backend.selection.title || (window.backend.selectionStatus === "inspecting"
                                ? (window.backend.selectionLoading ? qsTr("Loading track…") : qsTr("Track details unavailable"))
                                : qsTr("Selected track"))
                        }
                        PageLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: {
                                if (window.backend.selectionStatus === "inspecting")
                                    return window.backend.selection.subtitle || ""
                                if (window.backend.selectionStatus === "failed")
                                    return qsTr("Soloist could not perform that action.")
                                if (window.backend.selectionStatus === "waiting" && window.track.ready)
                                    return qsTr("Waiting to start playback…")
                                return window.playerMessage(window.backend.playerStatus)
                            }
                            visible: text !== ""
                        }
                    }
                    RowButton {
                        diameter: 28
                        glyph: "close"
                        text: window.backend.selectionStatus === "inspecting" ? qsTr("Dismiss track details") : qsTr("Dismiss playback request")
                        visible: window.backend.selectionStatus !== ""
                        onClicked: window.backend.dismissSelection()
                    }
                    Button {
                        visible: ["player_missing", "player_expired", "player_timeout", "player_connection"].includes(window.backend.playerStatus)
                        text: qsTr("Retry")
                        onClicked: window.backend.startPlayer()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: window.keyEntry

                    TextField {
                        id: keyField
                        Layout.fillWidth: true
                        echoMode: TextField.Password
                        maximumLength: 8192
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                        placeholderText: qsTr("Soloist API key")
                        onAccepted: saveKey.clicked()
                    }
                    Button {
                        text: qsTr("Get API key")
                        onClicked: Qt.openUrlExternally("https://developer.spotify.com/dashboard/soloist")
                    }
                    Button {
                        id: saveKey
                        text: qsTr("Save key")
                        enabled: keyField.text.trim() !== ""
                        onClicked: if (enabled) {
                            window.backend.saveKey(keyField.text)
                            keyField.clear()
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            color: window.surface

            Rectangle { width: parent.width; height: 1; color: window.line }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: window.inset
                anchors.rightMargin: window.inset
                spacing: window.width >= 660 ? 16 : 8

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: Math.min(245, window.width * 0.27)
                    Layout.minimumWidth: 110
                    spacing: 9

                    Artwork {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 46
                        source: window.track.art || ""
                        ink: window.theme.foreground
                        surround: window.surface
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            text: window.trackTitle
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        PageLabel {
                            Layout.fillWidth: true
                            text: window.track.artist || ""
                            visible: text !== ""
                        }
                    }
                }

                Transport {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 170
                    Layout.preferredWidth: 340
                    Layout.maximumWidth: 376
                    backend: window.backend
                    foreground: window.theme.foreground
                    accent: window.theme.accent
                    accentForeground: window.theme.accentForeground
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: Math.min(180, window.width * 0.23)
                    Layout.minimumWidth: 70
                    spacing: 2
                    Item { Layout.fillWidth: true }
                    RowButton {
                        glyph: window.backend.favorite === 1 ? "saved" : "add"
                        text: window.backend.favorite === 1 ? qsTr("Remove from Liked Songs") : qsTr("Save to Liked Songs")
                        enabled: window.backend.favorite >= 0 && !window.backend.webBusy && !window.backend.webCoolingDown
                        onClicked: window.backend.toggleFavorite()
                    }
                    RowButton {
                        glyph: "expand"
                        text: qsTr("Now Playing")
                        onClicked: window.nowPlayingExpanded = true
                    }
                }
            }
        }
    }

    Loader {
        anchors.fill: parent
        active: window.nowPlayingExpanded
        focus: active
        sourceComponent: NowPlaying {
            backend: window.backend
            theme: window.theme
            displayTitle: window.trackTitle
            onExitRequested: window.nowPlayingExpanded = false
        }
    }
}
