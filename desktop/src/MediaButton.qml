import QtQuick
import QtQuick.Controls

AbstractButton {
    id: button

    required property string glyph
    required property color foreground
    required property color emphasis
    property color emphasisText: foreground
    property bool primary: false
    property int diameter: 34

    implicitWidth: diameter
    implicitHeight: diameter
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.name: text
    ToolTip.text: text
    ToolTip.delay: 500
    ToolTip.visible: hovered && text !== ""

    contentItem: Item {
        MediaIcon {
            anchors.centerIn: parent
            name: button.glyph
            color: button.primary ? button.emphasisText : button.foreground
            opacity: button.enabled ? 1 : 0.34
        }
    }
    background: Rectangle {
        radius: button.diameter / 2
        border.width: button.visualFocus ? 2 : 0
        border.color: button.primary ? button.emphasisText : button.emphasis
        color: button.primary ? Qt.alpha(button.emphasis, button.enabled ? (button.down ? 0.78 : 1) : 0.25)
                              : Qt.alpha(button.foreground, button.down ? 0.17 : button.hovered ? 0.09 : 0)
    }
}
