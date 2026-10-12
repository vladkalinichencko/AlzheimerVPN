import QtQuick
import Style 1.0

import "../Controls2"

LabelWithButtonType {
    id: root

    property string hostname
    property string addresses
    property bool succeeded: false
    property bool failed: false

    signal removeRequested()

    text: hostname
    descriptionText: addresses
    hideDescription: false
    rightImageSource: "qrc:/images/controls/trash.svg"
    rightImageColor: AmneziaStyle.color.paleGray
    buttonImageSource: failed ? "qrc:/images/controls/close.svg"
                             : succeeded ? "qrc:/images/controls/check.svg" : ""
    Binding {
        target: root.eyeButton
        property: "imageColor"
        value: root.failed ? AmneziaStyle.color.vibrantRed : AmneziaStyle.color.vibrantGreen
    }
    Component.onCompleted: {
        eyeButton.isFocusable = false
        eyeButton.hoverEnabled = false
    }
    clickedFunction: function() { root.removeRequested() }
}
