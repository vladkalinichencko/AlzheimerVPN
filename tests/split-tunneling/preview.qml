import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Style 1.0
import "../../client/ui/qml/Pages2"
import "../../client/ui/qml/Controls2"

Window {
    visible: true
    width: 620
    height: content.implicitHeight + 48
    color: AmneziaStyle.color.midnightBlack

    ColumnLayout {
        id: content
        width: parent.width - 32
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        spacing: 8

        Label {
            Layout.leftMargin: 16
            text: "Раздельное туннелирование"
            font.pixelSize: 26
            color: AmneziaStyle.color.paleGray
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            text: "Адреса из списка должны открываться без VPN"
            font.pixelSize: 16
            color: AmneziaStyle.color.goldenApricot
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: [
                {host: "api.tinkoff.ru", ips: "178.130.128.21", status: "IPv4-адреса получены", detail: "", failed: false, canCheck: true, checking: false},
                {host: "rpc.alice.yandex.ru", ips: "", status: "Получаем адреса...", detail: "", failed: false, canCheck: true, checking: true},
                {host: "missing.invalid", ips: "", status: "Не удалось получить адреса", detail: "Host not found", failed: true, canCheck: true, checking: false},
                {host: "*.yandex.ru", ips: "", status: "Правило для доменов по маске", detail: "Адреса появятся при обращении к подходящим доменам.", failed: false, canCheck: false, checking: false},
                {host: "kinopoisk.ru", ips: "213.180.199.9", status: "Адреса сохранены, проверка не выполнена", detail: "", failed: false, canCheck: true, checking: false},
                {host: "192.0.2.7", ips: "", status: "Задан IP-адрес или подсеть", detail: "", failed: false, canCheck: false, checking: false}
            ]
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                SplitTunnelSiteRow {
                    Layout.fillWidth: true
                    hostname: modelData.host
                    addresses: modelData.ips
                    succeeded: modelData.ips.length > 0 || modelData.host === "192.0.2.7"
                    failed: modelData.failed
                }
                DividerType { Layout.fillWidth: true }
            }
        }
    }
}
