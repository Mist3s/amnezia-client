import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    Component.onCompleted: ManagedConfigController.openServer(ServersUiController.processedServerId)

    Connections {
        target: ManagedConfigController

        function onManualRefreshFinished(message) {
            PageController.showNotificationMessage(message)
        }
    }

    FlickableType {
        id: flickable

        anchors.top: parent.top
        anchors.bottom: buttons.top

        contentHeight: content.implicitHeight

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 0

            NavigationBarType {
                Layout.fillWidth: true

                title: qsTr("Config auto-update")
                description: qsTr("The app fetches the current config from your backend")
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 24

                spacing: 12

                TextFieldType {
                    id: backendUrls

                    Layout.fillWidth: true

                    headerText: qsTr("Backend address")
                    helperText: qsTr("HTTPS only. Several addresses can be separated by commas")

                    textField.text: ManagedConfigController.backendUrls
                }

                TextFieldType {
                    id: deviceKey

                    Layout.fillWidth: true

                    headerText: qsTr("Device key")
                    passwordToggle: true

                    textField.text: ManagedConfigController.deviceKey
                }

                CaptionTextType {
                    Layout.fillWidth: true

                    wrapMode: Text.WordWrap
                    text: ManagedConfigController.statusText
                }
            }
        }
    }

    ColumnLayout {
        id: buttons

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 8 + PageController.safeAreaBottomMargin

        spacing: 12

        ButtonType {
            Layout.fillWidth: true

            variant: ButtonType.Secondary
            text: qsTr("Update config")
            loading: ManagedConfigController.isRefreshing
            enabled: !ManagedConfigController.isRefreshing

            onClicked: ManagedConfigController.refresh()
        }

        ButtonType {
            Layout.fillWidth: true

            text: qsTr("Save")
            enabled: !ManagedConfigController.isRefreshing

            onClicked: {
                backendUrls.errorText = ""
                deviceKey.errorText = ""

                if (!ManagedConfigController.save(backendUrls.textField.text, deviceKey.textField.text)) {
                    if (deviceKey.textField.text.trim() === "") {
                        deviceKey.errorText = qsTr("Enter the device key")
                    } else {
                        backendUrls.errorText = qsTr("Enter an https:// address")
                    }
                    return
                }
                PageController.showNotificationMessage(qsTr("Settings saved"))
                ManagedConfigController.refresh()
            }
        }
    }
}
