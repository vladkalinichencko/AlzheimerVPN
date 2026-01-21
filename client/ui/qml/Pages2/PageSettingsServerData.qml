import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ProtocolEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"
import "../Config"

PageType {
    id: root

    signal lastItemTabClickedSignal()

    property bool isServerWithWriteAccess: ServersModel.isProcessedServerHasWriteAccess()
    property string selectedContainerValue: "all"

    Connections {
        target: InstallController

        function onScanServerFinished(isInstalledContainerFound) {
            var message = ""
            if (isInstalledContainerFound) {
                message = qsTr("All installed containers have been added to the application")
            } else {
                message = qsTr("No new installed containers found")
            }

            PageController.showErrorMessage(message)
        }

        function onRebootProcessedServerFinished(finishedMessage) {
            PageController.showNotificationMessage(finishedMessage)
        }

        function onRemoveAllContainersFinished(finishedMessage) {
            PageController.closePage() // close deInstalling page
            PageController.showNotificationMessage(finishedMessage)
        }

        function onRemoveProcessedContainerFinished(finishedMessage) {
            PageController.closePage() // close deInstalling page
            PageController.closePage() // close page with remove button
            PageController.showNotificationMessage(finishedMessage)
        }
    }

    Connections {
        target: SettingsController
        function onChangeSettingsFinished(finishedMessage) {
            PageController.showNotificationMessage(finishedMessage)
        }
    }

    Connections {
        target: ServersModel

        function onProcessedServerIndexChanged() {
            root.isServerWithWriteAccess = ServersModel.isProcessedServerHasWriteAccess()
        }
    }

    ListViewType {
        id: listView

        anchors.fill: parent

        model: serverActions

        delegate: ColumnLayout {
            width: listView.width

            // Кастомная секция для backup
            ColumnLayout {
                Layout.fillWidth: true
                visible: isVisible && isBackupSection

                spacing: 16

                Header2Type {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 8
                    
                    headerText: qsTr("Backup & Restore")
                }

                ParagraphTextType {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    
                    text: qsTr("Create and restore server configuration backups")
                    color: AmneziaStyle.color.mutedGray
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    spacing: 12

                    LabelTextType {
                        text: qsTr("Select containers:")
                    }

                    DropDownType {
                        id: containerSelector
                        Layout.fillWidth: true

                        objectName: "containerSelector"
                        descriptionText: qsTr("Choose which containers to backup")
                        headerText: qsTr("Containers")
                        
                        text: "All containers"
                        
                        drawerHeight: 0.4375
                        drawerParent: root

                        listView: ListViewWithRadioButtonType {
                            id: containerSelectorListView
                            rootWidth: root.width
                            
                            model: ListModel {
                                id: containersModel
                                ListElement { name: "All containers"; value: "all" }
                                ListElement { name: "OpenVPN"; value: "amnezia-openvpn" }
                                ListElement { name: "WireGuard"; value: "amnezia-wireguard" }
                                ListElement { name: "AmneziaWG (legacy)"; value: "amnezia-awg" }
                                ListElement { name: "AmneziaWG 2"; value: "amnezia-awg2" }
                                ListElement { name: "Xray"; value: "amnezia-xray" }
                                ListElement { name: "IKEv2"; value: "amnezia-ipsec" }
                                ListElement { name: "Cloak"; value: "amnezia-cloak" }
                                ListElement { name: "ShadowSocks"; value: "amnezia-shadowsocks" }
                            }

                            clickedFunction: function() {
                                if (containerSelectorListView.selectedText) {
                                    containerSelector.text = containerSelectorListView.selectedText
                                }
                                // Сохранить выбранное значение
                                var index = containerSelectorListView.selectedIndex
                                if (index >= 0 && index < containersModel.count) {
                                    root.selectedContainerValue = containersModel.get(index).value
                                }
                                containerSelector.closeTriggered()
                            }

                            Component.onCompleted: {
                                containerSelectorListView.selectedIndex = 0
                                root.selectedContainerValue = "all"
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        BasicButtonType {
                            Layout.fillWidth: true
                            text: qsTr("Create Backup")
                            
                            clickedFunc: function() {
                                createBackup(false) // false = не скачивать автоматически
                            }
                        }

                        BasicButtonType {
                            Layout.fillWidth: true
                            text: qsTr("Backup & Download")
                            
                            clickedFunc: function() {
                                createBackup(true) // true = скачать после создания
                            }
                        }
                    }

                    BasicButtonType {
                        Layout.fillWidth: true
                        text: qsTr("Restore Backup")
                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: Qt.rgba(1, 1, 1, 0.08)
                        pressedColor: Qt.rgba(1, 1, 1, 0.12)
                        disabledColor: AmneziaStyle.color.mutedGray
                        textColor: AmneziaStyle.color.goldenApricot

                        clickedFunc: function() {
                            restoreBackup()
                        }
                    }

                    BasicButtonType {
                        Layout.fillWidth: true
                        text: qsTr("Manage Backups")
                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: Qt.rgba(1, 1, 1, 0.08)
                        pressedColor: Qt.rgba(1, 1, 1, 0.12)
                        disabledColor: AmneziaStyle.color.mutedGray
                        textColor: AmneziaStyle.color.goldenApricot

                        clickedFunc: function() {
                            manageBackups()
                        }
                    }
                }
            }

            // Обычные кнопки для других действий
            LabelWithButtonType {
                Layout.fillWidth: true

                visible: isVisible && !isBackupSection

                text: title
                descriptionText: description
                textColor: tColor

                clickedFunction: function() {
                    clickedHandler()
                }
            }

            DividerType {
                visible: isVisible
            }
        }
    }

    property list<QtObject> serverActions: [
        check,
        backupSection,
        reboot,
        remove,
        clear,
        reset,
        switch_to_premium,
    ]

    QtObject {
        id: check

        property bool isVisible: root.isServerWithWriteAccess
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Check the server for previously installed Amnezia services")
        readonly property string description: qsTr("Add them to the application if they were not displayed")
        readonly property var tColor: AmneziaStyle.color.paleGray
        readonly property var clickedHandler: function() {
            PageController.showBusyIndicator(true)
            InstallController.scanServerForInstalledContainers()
            PageController.showBusyIndicator(false)
        }
    }

    QtObject {
        id: backupSection

        property bool isVisible: root.isServerWithWriteAccess
        readonly property bool isBackupSection: true
        readonly property string title: ""
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.paleGray
        readonly property var clickedHandler: function() {}
    }

    QtObject {
        id: reboot

        property bool isVisible: root.isServerWithWriteAccess
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Reboot server")
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.vibrantRed
        readonly property var clickedHandler: function() {
            var headerText = qsTr("Do you want to reboot the server?")
            var descriptionText = qsTr("The reboot process may take approximately 30 seconds. Are you sure you wish to proceed?")
            var yesButtonText = qsTr("Continue")
            var noButtonText = qsTr("Cancel")

            var yesButtonFunction = function() {
                if (ServersModel.isDefaultServerCurrentlyProcessed() && ConnectionController.isConnected) {
                    PageController.showNotificationMessage(qsTr("Cannot reboot server during active connection"))
                } else {
                    PageController.showBusyIndicator(true)
                    InstallController.rebootProcessedServer()
                    PageController.showBusyIndicator(false)
                }
            }
            var noButtonFunction = function() {

            }

            showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
        }
    }

    QtObject {
        id: remove

        property bool isVisible: true
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Remove server from application")
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.vibrantRed
        readonly property var clickedHandler: function() {
            var headerText = qsTr("Do you want to remove the server from application?")
            var descriptionText = qsTr("All installed AmneziaVPN services will still remain on the server.")
            var yesButtonText = qsTr("Continue")
            var noButtonText = qsTr("Cancel")

            var yesButtonFunction = function() {
                if (ServersModel.isDefaultServerCurrentlyProcessed() && ConnectionController.isConnected) {
                    PageController.showNotificationMessage(qsTr("Cannot remove server during active connection"))
                } else {
                    PageController.showBusyIndicator(true)
                    InstallController.removeProcessedServer()
                    PageController.showBusyIndicator(false)
                }
            }
            var noButtonFunction = function() {

            }

            showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
        }
    }

    QtObject {
        id: clear

        property bool isVisible: root.isServerWithWriteAccess
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Clear server from Amnezia software")
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.vibrantRed
        readonly property var clickedHandler: function() {
            var headerText = qsTr("Do you want to clear server from Amnezia software?")
            var descriptionText = qsTr("All users whom you shared a connection with will no longer be able to connect to it.")
            var yesButtonText = qsTr("Continue")
            var noButtonText = qsTr("Cancel")

            var yesButtonFunction = function() {
                if (ServersModel.isDefaultServerCurrentlyProcessed() && ConnectionController.isConnected) {
                    PageController.showNotificationMessage(qsTr("Cannot clear server from Amnezia software during active connection"))
                } else {
                    PageController.goToPage(PageEnum.PageDeinstalling)
                    InstallController.removeAllContainers()
                }
            }
            var noButtonFunction = function() {

            }

            showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
        }
    }

    QtObject {
        id: reset

        property bool isVisible: ServersModel.getProcessedServerData("isServerFromTelegramApi")
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Reset API config")
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.vibrantRed
        readonly property var clickedHandler: function() {
            var headerText = qsTr("Do you want to reset API config?")
            var descriptionText = ""
            var yesButtonText = qsTr("Continue")
            var noButtonText = qsTr("Cancel")

            var yesButtonFunction = function() {
                if (ServersModel.isDefaultServerCurrentlyProcessed() && ConnectionController.isConnected) {
                    PageController.showNotificationMessage(qsTr("Cannot reset API config during active connection"))
                } else {
                    PageController.showBusyIndicator(true)
                    InstallController.removeApiConfig(ServersModel.processedIndex)
                    PageController.showBusyIndicator(false)
                }
            }
            var noButtonFunction = function() {

            }

            showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
        }
    }

    QtObject {
        id: switch_to_premium

        property bool isVisible: ServersModel.getProcessedServerData("isServerFromTelegramApi") && ServersModel.processedServerIsPremium
        readonly property bool isBackupSection: false
        readonly property string title: qsTr("Switch to the new Amnezia Premium subscription")
        readonly property string description: ""
        readonly property var tColor: AmneziaStyle.color.vibrantRed
        readonly property var clickedHandler: function() {
            PageController.goToPageHome()
            ApiPremV1MigrationController.showMigrationDrawer()
        }
    }

    // ============ Backup Functions ============

    function getServerCredentials() {
        var index = ServersModel.processedIndex
        return ServersModel.getServerCredentials(index)
    }

    function getSelectedContainerValue() {
        return root.selectedContainerValue
    }

    property bool downloadAfterCreate: false

    function createBackup(shouldDownload) {
        downloadAfterCreate = shouldDownload || false
        
        var headerText = shouldDownload ? 
            qsTr("Create backup and download to device?") :
            qsTr("Create server configuration backup?")
        var descriptionText = shouldDownload ?
            qsTr("Backup will be created on server and automatically downloaded to your device") :
            qsTr("This will create a backup of your server containers configuration on the server")
        var yesButtonText = qsTr("Create")
        var noButtonText = qsTr("Cancel")

        var yesButtonFunction = function() {
            PageController.showBusyIndicator(true)
            
            var credentials = getServerCredentials()
            var containerValue = getSelectedContainerValue()
            
            if (containerValue === "all") {
                ServersBackupController.createBackup(credentials)
            } else {
                ServersBackupController.createBackupByName(credentials, containerValue)
            }
        }
        var noButtonFunction = function() {}

        showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
    }

    property string selectedBackupForRestore: ""

    function restoreBackup() {
        // Шаг 1: Выбрать backup файл на устройстве
        // На Android используем "*.gz" для MIME типа application/gzip
        // На Desktop используем "Backup files (*.tar.gz *.backup)"
        var filter = GC.isMobile() ? "*.gz" : "Backup files (*.tar.gz *.backup)"
        var localPath = SystemController.getFileName(
            qsTr("Select Backup to Restore"), 
            filter,
            "",
            false, // openMode
            ""
        )
        
        if (!localPath || localPath.length === 0) {
            return // Пользователь отменил
        }
        
        selectedBackupForRestore = localPath
        
        // Шаг 2: Подтверждение восстановления
        var headerText = qsTr("Restore server configuration?")
        var descriptionText = qsTr("Selected backup will be uploaded to server and restored. Current configuration will be overwritten. All containers will be restarted.")
        var yesButtonText = qsTr("Restore")
        var noButtonText = qsTr("Cancel")

        var yesButtonFunction = function() {
            PageController.showBusyIndicator(true)
            
            var credentials = getServerCredentials()
            
            // Загрузить backup на сервер
            ServersBackupController.uploadBackup(credentials, selectedBackupForRestore)
        }
        var noButtonFunction = function() {
            selectedBackupForRestore = ""
        }

        showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
    }

    function manageBackups() {
        PageController.showNotificationMessage(qsTr("Backup management page will be available in the next update"))
    }

    // ============ Backup Controller Connections ============

    property string lastCreatedBackupFilename: ""
    property string lastUploadedBackupFilename: ""

    Connections {
        target: ServersBackupController

        function onBackupCreated(backupFilename) {
            lastCreatedBackupFilename = backupFilename
            
            if (downloadAfterCreate) {
                // Скачать backup на устройство
                var credentials = getServerCredentials()
                
                // Автоматически скачиваем:
                var localPath = backupFilename
                PageController.showNotificationMessage(qsTr("Backup created. Downloading to device..."))
                ServersBackupController.downloadBackup(credentials, backupFilename, localPath)
                
                downloadAfterCreate = false
            } else {
                PageController.showBusyIndicator(false)
                PageController.showNotificationMessage(qsTr("Backup created successfully: %1").arg(backupFilename))
            }
        }

        function onBackupDownloaded(localPath) {
            PageController.showBusyIndicator(false)
            console.log("Backup downloaded to:", localPath)
            
            // Удалить backup с сервера после успешного скачивания
            if (lastCreatedBackupFilename && lastCreatedBackupFilename.length > 0) {
                var credentials = getServerCredentials()
                ServersBackupController.deleteBackup(credentials, lastCreatedBackupFilename)
                console.log("Deleting backup from server:", lastCreatedBackupFilename)
            }
            
            PageController.showNotificationMessage(qsTr("Backup downloaded successfully!\n\nSaved to:\n%1").arg(localPath))
        }

        function onBackupUploaded(serverPath) {
            // Backup загружен на сервер, теперь восстановить
            PageController.showNotificationMessage(qsTr("Backup uploaded. Restoring configuration..."))
            
            // Извлечь имя файла из пути и сохранить для последующего удаления
            var backupFilename = serverPath.split('/').pop()
            lastUploadedBackupFilename = backupFilename
            
            var credentials = getServerCredentials()
            ServersBackupController.restoreBackup(credentials, backupFilename)
        }

        function onBackupRestored() {
            PageController.showBusyIndicator(false)
            
            // Удалить backup с сервера после успешного восстановления
            /* if (lastUploadedBackupFilename && lastUploadedBackupFilename.length > 0) {
                var credentials = getServerCredentials()
                ServersBackupController.deleteBackup(credentials, lastUploadedBackupFilename)
                console.log("Deleting backup from server after restore:", lastUploadedBackupFilename)
                lastUploadedBackupFilename = ""
            }*/
            
            selectedBackupForRestore = ""
            PageController.showNotificationMessage(qsTr("Backup restored successfully! Containers are restarting..."))
        }

        function onProgressChanged(percent, message) {
            // TODO: Show progress in UI
            console.log("Backup progress:", percent, "%", message)
        }

        function onErrorOccurred(errorMessage, errorCode) {
            PageController.showBusyIndicator(false)
            PageController.showErrorMessage(qsTr("Backup error: %1").arg(errorMessage))
        }
    }
}
