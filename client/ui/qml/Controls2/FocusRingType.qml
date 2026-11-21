import QtQuick
import QtQuick.Effects

import Style 1.0

Item {
    id: root

    property Item target: parent
    property real radius: 16
    property bool accent: false
    readonly property bool isRemoteControlled: Qt.platform.os === "android"

    anchors.fill: target
    anchors.margins: -3

    Loader {
        anchors.fill: parent

        active: root.visible

        sourceComponent: Item {
            MultiEffect {
                anchors.fill: ring
                source: ring
                shadowEnabled: true
                shadowColor: ring.border.color
                shadowBlur: root.isRemoteControlled ? 1.0 : 0.5
                shadowHorizontalOffset: 0
                shadowVerticalOffset: 0
            }

            Rectangle {
                id: ring

                anchors.fill: parent

                radius: root.radius + 3
                color: AmneziaStyle.color.transparent
                border.width: root.isRemoteControlled ? 3 : 1
                border.color: root.accent ? AmneziaStyle.color.accentLight : AmneziaStyle.color.borderEmphasis
            }
        }
    }
}
