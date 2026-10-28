/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#ifndef RELAYTUNNELSOCKET_H
#define RELAYTUNNELSOCKET_H

#include <QSslSocket>
#include <QTimer>

#include "kdeconnectcore_export.h"
#include "relayprotocol.h"

// Data connection to a relay tunnel: sends the attach line, then runs end-to-end TLS with the peer device and
// checks the peer certificate against a pin before anything is read from it.
class KDECONNECTCORE_EXPORT RelayTunnelSocket : public QSslSocket
{
    Q_OBJECT

public:
    enum class Role {
        TlsServer,
        TlsClient,
    };

    explicit RelayTunnelSocket(QObject *parent = nullptr);

    void open(const QString &host,
              quint16 port,
              const QString &token,
              Role role,
              const QSslCertificate &localCertificate,
              const QSslKey &privateKey,
              const RelayProtocol::Target &pin);

    bool isEstablished() const
    {
        return m_established;
    }

    static const int ESTABLISH_TIMEOUT_MS = 30000;

Q_SIGNALS:
    void established();
    void failed();

private:
    void fail();

    QString m_token;
    Role m_role = Role::TlsClient;
    RelayProtocol::Target m_pin;
    QTimer m_timeout;
    bool m_established = false;
    bool m_failed = false;
};

#endif
