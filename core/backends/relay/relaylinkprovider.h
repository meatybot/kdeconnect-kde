/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef RELAYLINKPROVIDER_H
#define RELAYLINKPROVIDER_H

#include <QHash>
#include <QMap>
#include <QPointer>
#include <QSet>

#include "backends/linkprovider.h"
#include "kdeconnectcore_export.h"
#include "relayconnection.h"
#include "relaydevicelink.h"
#include "relaytunnelsocket.h"

// Connects to devices on other networks through a user-configured relay server. See docs/specs/relay.md
class KDECONNECTCORE_EXPORT RelayLinkProvider : public LinkProvider
{
    Q_OBJECT

public:
    explicit RelayLinkProvider(bool disabled = false);
    ~RelayLinkProvider() override;

    QString id() const override
    {
        return QStringLiteral("relay");
    }
    QString displayName() const override
    {
        return i18nc("@info", "Relay server");
    }
    int priority() override
    {
        return 5;
    }
    void enable() override;
    void disable() override;

    // "host[:port]", empty to stop using a relay. Persisted in KdeConnectConfig.
    bool setServer(const QString &address);
    QString server() const;
    RelayConnection::Status status() const
    {
        return m_connection.status();
    }
    QString relayKey() const
    {
        return m_connection.key();
    }

    // Ask the relay for a link to the device with this relay key or fingerprint
    bool connectTo(const RelayProtocol::Target &target);
    // Fingerprints of paired devices, to be told when they are online
    void refreshWatchList();

    // Used by RelayDeviceLink
    QString requestPayloadTunnel(RelayDeviceLink *link);
    RelayTunnelSocket *openTunnel(const QString &token, RelayTunnelSocket::Role role, const RelayProtocol::Target &pin, QObject *parent);

#ifdef BUILD_TESTING
    RelayConnection *testConnection()
    {
        return &m_connection;
    }
#endif

public Q_SLOTS:
    void onStart() override;
    void onStop() override;
    void onNetworkChange() override;
    void onLinkDestroyed(const QString &deviceId, DeviceLink *oldPtr) override;

Q_SIGNALS:
    void statusChanged();

private:
    void applyConfig();
    void tunnelReady(const QString &requestId, const QString &token, const QString &purpose, const QString &peerFingerprint);
    void requestFailed(const QString &requestId, const QString &code);
    void startLinkTunnel(const QString &token, RelayTunnelSocket::Role role, const RelayProtocol::Target &pin);
    void exchangeIdentity(RelayTunnelSocket *socket);
    void identityReceived(RelayTunnelSocket *socket);
    void addLink(RelayTunnelSocket *socket, const DeviceInfo &deviceInfo);
    bool hasLinkWithFingerprint(const QString &fingerprint) const;

    RelayConnection m_connection;
    bool m_disabled;
    bool m_started = false;
    QMap<QString, RelayDeviceLink *> m_links;
    QHash<QString, RelayProtocol::Target> m_pendingLinkRequests; // requestId -> pin
    QHash<QString, QPointer<RelayDeviceLink>> m_pendingPayloadRequests; // requestId -> link
    QSet<RelayTunnelSocket *> m_pendingTunnels; // Not verified yet, only compared by address
};

#endif
