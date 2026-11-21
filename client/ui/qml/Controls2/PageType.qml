import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Config"

Item {
    id: root

    property StackView stackView: StackView.view
    property bool focusOnActivation: true
    property Item initialFocusItem: root
    property bool isTabBarHidden: false

    Rectangle {
        anchors.fill: parent
        z: -1

        color: AmneziaStyle.color.bgBase
    }

    function focusFirstControl() {
        if (root.StackView.status !== StackView.Active) {
            return
        }
        const item = root.initialFocusItem === root ? root.nextItemInFocusChain(true) : root.initialFocusItem
        item.forceActiveFocus(Qt.TabFocusReason)
    }

    StackView.onActivated: {
        if (!root.focusOnActivation) {
            return
        }
        for (let item = root.Window.activeFocusItem; item; item = item.parent) {
            if (item === root.Overlay.overlay) {
                return
            }
        }
        if (SettingsController.isOnTv()) {
            Qt.callLater(root.focusFirstControl)
            return
        }
        root.initialFocusItem.forceActiveFocus()
    }
}
