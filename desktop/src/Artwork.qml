import QtQuick
import QtQuick.Shapes

Rectangle {
    id: cover

    property alias source: image.source
    property string glyph: "note"
    property int decodeSize: 128
    required property color ink
    required property color surround

    radius: 6
    color: Qt.tint(surround, Qt.alpha(ink, 0.08))

    MediaIcon {
        anchors.centerIn: parent
        width: Math.min(56, cover.width * 0.43)
        height: width
        name: cover.glyph
        color: Qt.alpha(cover.ink, 0.48)
        visible: image.status !== Image.Ready
    }

    Image {
        id: image
        anchors.fill: parent
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: cover.decodeSize
        sourceSize.height: cover.decodeSize
        visible: status === Image.Ready
    }

    Shape {
        anchors.fill: parent
        visible: image.visible
        ShapePath {
            strokeWidth: -1
            fillColor: cover.surround
            fillRule: ShapePath.OddEvenFill
            PathRectangle { width: cover.width; height: cover.height }
            PathRectangle { width: cover.width; height: cover.height; radius: cover.radius }
        }
    }
}
