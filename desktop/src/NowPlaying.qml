pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmarchySpotify

FocusScope {
    id: stage

    required property Backend backend
    required property Theme theme
    required property string displayTitle
    signal exitRequested()

    readonly property var track: backend.playback
    readonly property bool wide: width >= 680 && width >= height * 1.2
    readonly property color baseColor: "#101318"
    readonly property color ink: "#f6f7fa"
    readonly property color accent: theme.light ? Qt.lighter(theme.accent, 1.35) : theme.accent
    readonly property real artSide: wide ? Math.min(width * 0.42, height * 0.75, 480)
        : Math.max(56, Math.min(width * 0.68, height * 0.48, height - details.implicitHeight - 100, 400))

    Rectangle { anchors.fill: parent; color: stage.baseColor }

    Image {
        id: backdrop
        anchors.fill: parent
        source: stage.track.art || ""
        sourceSize.width: 48
        sourceSize.height: 48
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        opacity: status === Image.Ready ? 0.46 : 0
        Behavior on opacity { NumberAnimation { duration: 220 } }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#29101318" }
            GradientStop { position: 0.57; color: "#b4101318" }
            GradientStop { position: 1; color: stage.baseColor }
        }
    }

    MediaButton {
        id: exitButton
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 22
        glyph: "collapse"
        text: qsTr("Back to library")
        foreground: stage.ink
        emphasis: stage.accent
        diameter: 44
        focus: true
        onClicked: stage.exitRequested()
    }

    GridLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 56, stage.wide ? 1080 : 560)
        columns: stage.wide ? 2 : 1
        columnSpacing: stage.wide ? Math.min(72, stage.width * 0.055) : 0
        rowSpacing: stage.height >= 650 ? 28 : 14

        Artwork {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: stage.artSide
            Layout.preferredHeight: stage.artSide
            radius: 2
            source: stage.track.artHigh || stage.track.art || ""
            decodeSize: Math.min(1200, Math.max(640, Math.ceil(stage.artSide * Screen.devicePixelRatio / 128) * 128))
            surround: stage.baseColor
            ink: stage.ink
        }

        ColumnLayout {
            id: details

            Layout.fillWidth: true
            spacing: stage.height >= 650 ? 13 : 7

            Label {
                Layout.fillWidth: true
                horizontalAlignment: stage.wide ? Text.AlignLeft : Text.AlignHCenter
                text: stage.displayTitle
                textFormat: Text.PlainText
                color: stage.ink
                font.pixelSize: stage.wide ? Math.min(54, stage.width * 0.045) : Math.min(36, stage.width * 0.06)
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: stage.wide ? Text.AlignLeft : Text.AlignHCenter
                text: stage.track.artist || ""
                visible: text !== ""
                textFormat: Text.PlainText
                color: Qt.alpha(stage.ink, 0.76)
                font.pixelSize: stage.wide ? 20 : 16
                elide: Text.ElideRight
            }

            Transport {
                Layout.fillWidth: true
                Layout.topMargin: stage.height >= 650 ? 18 : 3
                backend: stage.backend
                foreground: stage.ink
                accent: stage.accent
                accentForeground: stage.theme.accentForeground
                expanded: true
            }
        }
    }
}
