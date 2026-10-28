// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kdeconnect
import org.kde.kdeconnect.app

Kirigami.ScrollablePage
{
    id: page
    title: i18nc("@title:window", "Settings")

    ColumnLayout
    {

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.gridUnit

            FormCard.FormTextFieldDelegate {
                id: deviceNameField
                text: announcedNameProperty.value
                onAccepted: DaemonDbusInterface.setAnnouncedName(text);
                label: i18n("Device name")

                DBusProperty {
                    id: announcedNameProperty
                    object: DaemonDbusInterface
                    read: "announcedName"
                    defaultValue: ""
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Backends")
        }

        FormCard.FormCard {
            DBusProperty {
                id: linkProvidersProperty
                object: DaemonDbusInterface
                read: "linkProviders"
                defaultValue: []
            }
            visible: linkProvidersProperty.value.length > 0

            Repeater {
                model: linkProvidersProperty.value

                FormCard.FormCheckDelegate {
                    required property string modelData

                    readonly property string displayName: modelData.split('|')[0]
                    readonly property string internalName: modelData.split('|')[1]

                    checked: modelData.split('|')[2] === 'enabled'
                    text: displayName

                    onToggled: DaemonDbusInterface.setLinkProviderState(internalName, checked);
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Relay server")
        }

        FormCard.FormCard {
            DBusProperty {
                id: relayServerProperty
                object: DaemonDbusInterface
                read: "relayServer"
                defaultValue: ""
            }
            DBusProperty {
                id: relayStatusProperty
                object: DaemonDbusInterface
                read: "relayStatus"
                defaultValue: "disabled"
            }
            DBusProperty {
                id: relayKeyProperty
                object: DaemonDbusInterface
                read: "relayKey"
                defaultValue: ""
            }
            // The fingerprint never changes while the daemon runs, fetch it once
            DBusAsyncResponse {
                id: selfFingerprintResponse
                property string value: ""
                autoDelete: false
                onSuccess: result => {
                    value = result;
                }
                Component.onCompleted: setPendingCall(DaemonDbusInterface.selfFingerprint())
            }

            DBusAsyncResponse {
                id: setRelayServerResponse
                autoDelete: false
                onSuccess: result => {
                    relayServerField.statusMessage = result ? "" : i18n("Invalid address");
                }
            }

            DBusAsyncResponse {
                id: connectResponse
                autoDelete: false
                onSuccess: result => {
                    if (result) {
                        connectTargetField.statusMessage = i18n("Pairing will be requested once the device is reachable. Compare the verification key on both devices.");
                        connectTargetField.text = "";
                    } else {
                        connectTargetField.statusMessage = i18n("Not a valid relay key or fingerprint");
                    }
                }
            }

            FormCard.FormTextFieldDelegate {
                id: relayServerField
                label: i18n("Relay server address")
                placeholderText: i18nc("@info:placeholder", "host:port")
                text: relayServerProperty.value
                status: Kirigami.MessageType.Error
                onAccepted: setRelayServerResponse.setPendingCall(DaemonDbusInterface.setRelayServer(text))
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormTextDelegate {
                text: i18n("Relay status")
                description: {
                    switch (relayStatusProperty.value) {
                    case "registered":
                        return i18nc("@info relay status", "Connected");
                    case "connecting":
                        return i18nc("@info relay status", "Connecting...");
                    case "offline":
                        return i18nc("@info relay status", "Unreachable, retrying");
                    default:
                        return i18nc("@info relay status", "Not in use");
                    }
                }
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormTextFieldDelegate {
                label: i18n("Your relay key")
                readOnly: true
                text: relayKeyProperty.value
                placeholderText: i18nc("@info:placeholder", "Available once connected to a relay server")
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormTextFieldDelegate {
                label: i18n("Your certificate fingerprint")
                readOnly: true
                text: selfFingerprintResponse.value
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormTextFieldDelegate {
                id: connectTargetField
                label: i18n("Connect to device")
                placeholderText: i18nc("@info:placeholder", "Relay key or certificate fingerprint")
                status: Kirigami.MessageType.Information
                onAccepted: connectResponse.setPendingCall(DaemonDbusInterface.connectToDevice(text))
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.gridUnit

            FormCard.FormButtonDelegate {
                text: i18n("About KDE Connect")
                onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutPage"))
                icon.name: 'kdeconnect'
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormButtonDelegate {
                text: i18n("About KDE")
                onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutKDEPage"))
                icon.name: 'kde'
            }
        }
    }

    footer: ToolBar {
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            Button {
                text: i18n("Close")
                display: AbstractButton.TextOnly
                onClicked: {
                    DaemonDbusInterface.setAnnouncedName(deviceNameField.text);
                    page.Kirigami.PageStack.closeDialog();
                }
            }
        }
    }
}
