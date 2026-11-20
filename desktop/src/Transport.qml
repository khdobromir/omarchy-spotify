pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmarchySpotify

ColumnLayout {
    id: transport

    required property Backend backend
    required property color foreground
    required property color accent
    required property color accentForeground
    property bool expanded: false
    readonly property var track: backend.playback
    readonly property bool ready: track.ready && !backend.commandBusy
    readonly property bool transitioning: backend.playbackTransition

    spacing: expanded ? 12 : 0

    function timestamp(ms: int): string {
        const seconds = Math.floor(ms / 1000)
        return Math.floor(seconds / 60).toString() + ":" + (seconds % 60).toString().padStart(2, "0")
    }

    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        spacing: transport.expanded ? 16 : 6

        MediaButton {
            glyph: "previous"
            text: qsTr("Previous track")
            foreground: transport.foreground
            emphasis: transport.accent
            enabled: transport.ready && !(transport.track.disallows ?? []).includes("skipping_prev")
            onClicked: transport.backend.skip(false)
        }
        MediaButton {
            diameter: transport.expanded ? 50 : 34
            glyph: transport.track.playing ? "pause" : "play"
            text: transport.track.playing ? qsTr("Pause") : qsTr("Play")
            foreground: transport.foreground
            emphasis: transport.accent
            emphasisText: transport.accentForeground
            primary: true
            enabled: transport.ready && !(transport.track.disallows ?? []).includes(transport.track.playing ? "pausing" : "resuming")
            onClicked: transport.backend.togglePlayback()
        }
        MediaButton {
            glyph: "next"
            text: qsTr("Next track")
            foreground: transport.foreground
            emphasis: transport.accent
            enabled: transport.ready && !(transport.track.disallows ?? []).includes("skipping_next")
            onClicked: transport.backend.skip(true)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 7

        Label {
            text: transport.transitioning ? "–:––" : transport.timestamp(seek.pressed ? seek.value : transport.backend.position)
            font.pixelSize: 11
            color: Qt.alpha(transport.foreground, 0.72)
        }

        Slider {
            id: seek
            property string dragTrack: ""
            property int dragPosition: 0
            Layout.fillWidth: true
            Layout.preferredHeight: 21
            leftPadding: 0
            rightPadding: 0
            to: Math.max(1, transport.track.duration)
            visible: !transport.transitioning
            enabled: visible && transport.track.ready && !(transport.track.disallows ?? []).includes("seeking") && transport.track.duration > 0 && (!transport.backend.commandBusy || pressed)
            Accessible.name: qsTr("Playback position")
            Binding on value {
                when: !seek.pressed
                value: transport.backend.position
            }
            onPressedChanged: {
                if (pressed) {
                    dragTrack = transport.track.uri
                    dragPosition = Math.round(value)
                } else if (dragTrack) {
                    transport.backend.seek(dragPosition, dragTrack)
                    dragTrack = ""
                }
            }
            onMoved: {
                if (pressed)
                    dragPosition = Math.round(value)
                else
                    transport.backend.seek(Math.round(value), transport.track.uri)
            }
            background: Rectangle {
                x: seek.leftPadding
                y: (seek.height - height) / 2
                width: seek.availableWidth
                height: 4
                radius: 2
                color: Qt.alpha(transport.foreground, 0.20)
                Rectangle {
                    height: parent.height
                    width: seek.position * parent.width
                    anchors.right: seek.mirrored ? parent.right : undefined
                    color: transport.accent
                    radius: 2
                }
            }
            handle: Rectangle {
                x: seek.leftPadding + seek.visualPosition * (seek.availableWidth - width)
                y: (seek.height - height) / 2
                width: 11
                height: width
                radius: width / 2
                color: transport.foreground
                visible: seek.enabled && (seek.hovered || seek.pressed || seek.visualFocus)
            }
        }

        Item {
            visible: transport.transitioning
            Layout.fillWidth: true
            Layout.preferredHeight: 21
        }

        Label {
            text: !transport.transitioning && transport.track.duration > 0 ? transport.timestamp(transport.track.duration) : "–:––"
            font.pixelSize: 11
            color: Qt.alpha(transport.foreground, 0.72)
        }
    }

    Label {
        text: qsTr("Spotify has not confirmed the command.")
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: Qt.alpha(transport.foreground, 0.72)
        font.pixelSize: 11
        visible: transport.track.commandUnconfirmed === true
        Layout.fillWidth: true
    }
}
