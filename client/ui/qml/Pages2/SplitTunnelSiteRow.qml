import QtQuick
import QtQuick.Controls.impl
import Style 1.0

import "../Controls2"

ListItemType {
    id: root

    property string hostname
    property string addresses
    property bool succeeded: false
    property bool failed: false

    signal removeRequested()

    text: hostname
    subtitle: addresses
    trailingIconSource: "qrc:/images/controls/trash.svg"

    trailingContent: IconImage {
        width: 24
        height: 24

        visible: root.succeeded || root.failed
        source: root.failed ? "qrc:/images/controls/close.svg" : "qrc:/images/controls/check.svg"
        sourceSize: Qt.size(24, 24)
        color: root.failed ? AmneziaStyle.color.statusDestructive : AmneziaStyle.color.statusSuccess
    }

    onClicked: root.removeRequested()
}
