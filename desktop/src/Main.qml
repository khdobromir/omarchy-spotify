pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmarchySpotify

ApplicationWindow {
    id: window

    required property Backend backend
    required property Theme theme
    readonly property var track: backend.playback
    readonly property bool canPlay: track.ready && !backend.commandBusy
    readonly property bool keyEntry: ["key_required", "key_write", "player_failed"].includes(backend.playerStatus)

    width: 1000
    height: 660
    minimumWidth: 740
    minimumHeight: 525
    title: "Omarchy Spotify"
    color: theme.background
    visible: true

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    function timeText(ms: int): string {
        const seconds = Math.floor(ms / 1000)
        return Math.floor(seconds / 60).toLocaleString(Qt.locale(), 'f', 0) + ":"
            + (seconds % 60).toString().padStart(2, "0")
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
        case "loading": return qsTr("Loading your library…")
        case "connecting": return qsTr("Approve library access in your browser.")
        case "setup_required": return qsTr("Save your Spotify Client ID in the Omarchy configuration first.")
        case "disconnected":
        case "reauthorize": return qsTr("Connect Spotify to browse your library.")
        case "quota":
        case "limited": return qsTr("Spotify paused requests. Try again later.")
        case "ok": return qsTr("This page is empty.")
        default: return qsTr("Could not load your Spotify library.")
        }
    }

    Shortcut {
        sequence: "Space"
        enabled: window.canPlay && !keyField.activeFocus
        onActivated: window.backend.togglePlayback()
    }

    Shortcut {
        sequence: "Return"
        enabled: window.canPlay && library.activeFocus && library.currentIndex >= 0
        onActivated: window.backend.play(window.backend.entries[library.currentIndex].uri)
    }

    Shortcut { sequences: [StandardKey.Quit]; onActivated: window.close() }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 28
        LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 300
            spacing: 12

            Label {
                text: qsTr("Your Library")
                font.pixelSize: 30
                font.bold: true
                Accessible.role: Accessible.Heading
            }

            TabBar {
                id: categories
                Layout.fillWidth: true
                enabled: !window.backend.webBusy
                currentIndex: ["tracks", "albums", "playlists"].indexOf(window.backend.category)

                TabButton { text: qsTr("Liked songs"); onClicked: window.backend.loadLibrary("tracks", 0) }
                TabButton { text: qsTr("Albums"); onClicked: window.backend.loadLibrary("albums", 0) }
                TabButton { text: qsTr("Playlists"); onClicked: window.backend.loadLibrary("playlists", 0) }
            }

            ListView {
                id: library
                focus: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: window.backend.entries
                reuseItems: true
                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: entry
                    required property int index
                    required property var modelData
                    width: ListView.view.width - 8
                    height: 54
                    enabled: window.canPlay
                    highlighted: ListView.isCurrentItem && library.activeFocus
                    Accessible.name: modelData.title
                    onClicked: {
                        library.currentIndex = index
                        window.backend.play(modelData.uri)
                    }

                    contentItem: Column {
                        spacing: 4
                        Label {
                            width: parent.width
                            text: entry.modelData.title
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.bold: true
                        }
                        Label {
                            width: parent.width
                            text: entry.modelData.subtitle
                            visible: text !== ""
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            opacity: 0.72
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: library.count === 0 || window.backend.libraryStatus !== "ok"
                    text: window.libraryMessage(window.backend.libraryStatus)
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Button {
                    text: qsTr("Connect Spotify")
                    visible: ["disconnected", "reauthorize", "denied", "auth_error", "auth_timeout", "browser_error"]
                        .includes(window.backend.libraryStatus)
                    enabled: !window.backend.webBusy
                    onClicked: window.backend.connectLibrary()
                }
                Button {
                    text: qsTr("Retry")
                    visible: ["unavailable", "forbidden", "action_failed", "quota", "limited"]
                        .includes(window.backend.libraryStatus)
                    enabled: !window.backend.webBusy
                    onClicked: window.backend.loadLibrary(window.backend.category, window.backend.offset)
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Previous page")
                    visible: window.backend.offset > 0
                    enabled: !window.backend.webBusy
                    onClicked: window.backend.loadLibrary(window.backend.category, Math.max(0, window.backend.offset - 20))
                }
                Button {
                    text: qsTr("Next page")
                    visible: window.backend.nextOffset >= 0
                    enabled: !window.backend.webBusy
                    onClicked: window.backend.loadLibrary(window.backend.category, window.backend.nextOffset)
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: Math.max(330, window.width * 0.37)
            Layout.fillHeight: true
            radius: window.theme.radius
            color: window.theme.colors.lighter_background || Qt.tint(window.theme.background, Qt.alpha(window.theme.foreground, 0.06))

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 8

                Label {
                    text: qsTr("On this computer")
                    color: window.theme.accent
                    font.bold: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 95
                    color: window.theme.background
                    radius: window.theme.radius

                    Image {
                        id: cover
                        anchors.fill: parent
                        anchors.margins: 12
                        fillMode: Image.PreserveAspectFit
                        source: window.track.art
                        sourceSize.width: 512
                        sourceSize.height: 512
                        asynchronous: true
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: cover.status !== Image.Ready
                        text: "♫"
                        color: window.theme.accent
                        font.pixelSize: 52
                        Accessible.ignored: true
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: window.track.title || qsTr("Nothing is playing")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.pixelSize: 22
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: [window.track.artist, window.track.album].filter(Boolean).join(" · ")
                    visible: text !== ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    opacity: 0.75
                }

                Slider {
                    id: seekBar
                    property string dragTrack: ""
                    property int dragPosition: 0
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, window.track.duration)
                    enabled: window.canPlay && window.track.duration > 0
                    Accessible.name: qsTr("Playback position")
                    Binding on value {
                        when: !seekBar.pressed
                        value: window.backend.position
                    }
                    onPressedChanged: {
                        if (pressed) {
                            dragTrack = window.track.uri
                            dragPosition = Math.round(value)
                        } else if (dragTrack) {
                            window.backend.seek(dragPosition, dragTrack)
                            dragTrack = ""
                        }
                    }
                    onMoved: {
                        if (pressed)
                            dragPosition = Math.round(value)
                        else
                            window.backend.seek(Math.round(value), window.track.uri)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 / %2").arg(window.timeText(seekBar.pressed ? seekBar.value : window.backend.position))
                        .arg(window.timeText(window.track.duration))
                    horizontalAlignment: Text.AlignHCenter
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 6

                    Button {
                        text: "⏮"
                        Accessible.name: qsTr("Previous track")
                        enabled: window.canPlay
                        onClicked: window.backend.skip(false)
                    }
                    Button {
                        text: window.track.playing ? "⏸" : "▶"
                        Accessible.name: window.track.playing ? qsTr("Pause") : qsTr("Play")
                        enabled: window.canPlay
                        onClicked: window.backend.togglePlayback()
                    }
                    Button {
                        text: "⏭"
                        Accessible.name: qsTr("Next track")
                        enabled: window.canPlay
                        onClicked: window.backend.skip(true)
                    }
                    Button {
                        text: window.backend.favorite === 1 ? "✓" : "+"
                        Accessible.name: window.backend.favorite === 1 ? qsTr("Remove from Liked Songs") : qsTr("Save to Liked Songs")
                        enabled: window.backend.favorite >= 0 && !window.backend.webBusy
                        onClicked: window.backend.toggleFavorite()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: window.playerMessage(window.backend.playerStatus)
                    visible: text !== ""
                    wrapMode: Text.WordWrap
                }

                TextField {
                    id: keyField
                    Layout.fillWidth: true
                    visible: window.keyEntry
                    echoMode: TextField.Password
                    maximumLength: 8192
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                    placeholderText: qsTr("Soloist API key")
                    onAccepted: if (text.trim()) {
                        window.backend.saveKey(text)
                        clear()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: window.keyEntry || ["player_missing", "player_expired", "player_timeout", "player_connection"]
                        .includes(window.backend.playerStatus)

                    Button {
                        visible: window.keyEntry
                        text: qsTr("Get API key")
                        onClicked: Qt.openUrlExternally("https://developer.spotify.com/dashboard/soloist")
                    }
                    Button {
                        visible: window.keyEntry
                        text: qsTr("Save key")
                        enabled: keyField.text.trim() !== ""
                        onClicked: {
                            window.backend.saveKey(keyField.text)
                            keyField.clear()
                        }
                    }
                    Button {
                        visible: !["key_required", "key_write"].includes(window.backend.playerStatus)
                        text: qsTr("Retry")
                        onClicked: window.backend.startPlayer()
                    }
                }
            }
        }
    }
}
