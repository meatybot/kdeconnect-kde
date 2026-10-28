/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "relaytunnelsocket.h"

#include <QNetworkProxy>
#include <QSslConfiguration>

#include "core_debug.h"

RelayTunnelSocket::RelayTunnelSocket(QObject *parent)
    : QSslSocket(parent)
{
    setProxy(QNetworkProxy::NoProxy);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(ESTABLISH_TIMEOUT_MS);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        qCWarning(KDECONNECT_CORE) << "Relay: tunnel timed out before the peer was verified";
        fail();
    });
}

void RelayTunnelSocket::open(const QString &host,
                             quint16 port,
                             const QString &token,
                             Role role,
                             const QSslCertificate &localCertificate,
                             const QSslKey &privateKey,
                             const RelayProtocol::Target &pin)
{
    m_token = token;
    m_role = role;
    m_pin = pin;

    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(localCertificate);
    config.setPrivateKey(privateKey);
    // Device certificates are self-signed: instead of chain verification the peer certificate is pinned
    config.setPeerVerifyMode(QSslSocket::QueryPeer);
    setSslConfiguration(config);

    connect(this, &QAbstractSocket::connected, this, [this]() {
        setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        write(RelayProtocol::encode({{QStringLiteral("type"), QStringLiteral("attach")}, {QStringLiteral("token"), m_token}}));
        if (m_role == Role::TlsServer) {
            startServerEncryption();
        } else {
            startClientEncryption();
        }
    });
    connect(this, &QSslSocket::encrypted, this, [this]() {
        if (!m_pin.isValid() || !RelayProtocol::matches(m_pin, peerCertificate())) {
            qCWarning(KDECONNECT_CORE) << "Relay: peer certificate does not match the expected" << (m_pin.kind == RelayProtocol::Target::Key ? "key" : "fingerprint");
            fail();
            return;
        }
        m_timeout.stop();
        m_established = true;
        Q_EMIT established();
    });
    connect(this, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (!m_established) {
            qCDebug(KDECONNECT_CORE) << "Relay: tunnel error" << error << errorString();
            fail();
        }
    });

    m_timeout.start();
    connectToHost(host, port);
}

void RelayTunnelSocket::fail()
{
    if (m_failed) {
        return;
    }
    m_failed = true;
    m_timeout.stop();
    abort();
    Q_EMIT failed();
}

#include "moc_relaytunnelsocket.cpp"
