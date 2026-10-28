/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef RELAYCONNECTION_H
#define RELAYCONNECTION_H

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>

#include "kdeconnectcore_export.h"
#include "relayprotocol.h"

// Control connection to a relay server: registration, tunnel requests and notifications.
// See docs/specs/relay.md
class KDECONNECTCORE_EXPORT RelayConnection : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Disabled,
        Connecting,
        Registered,
        Offline, // Waiting to retry
    };
    Q_ENUM(Status)

    RelayConnection(const QSslCertificate &certificate, const QSslKey &privateKey, QObject *parent = nullptr);
    ~RelayConnection() override;

    void setServer(const QString &host, quint16 port);
    QString host() const
    {
        return m_host;
    }
    quint16 port() const
    {
        return m_port;
    }

    void start();
    void stop();
    // Reconnect now if waiting to retry
    void retryNow();

    Status status() const
    {
        return m_status;
    }
    static QString statusString(Status status);

    // Key reported by the server for our certificate, empty until registered
    QString key() const
    {
        return m_key;
    }
    QSslCertificate certificate() const
    {
        return m_certificate;
    }
    QSslKey privateKey() const
    {
        return m_privateKey;
    }

    // Returns the request id, or an empty string if not registered
    QString requestTunnel(const RelayProtocol::Target &target, const QString &purpose);
    void setWatchList(const QStringList &fingerprints);

#ifdef BUILD_TESTING
    void testSetRetryInterval(int ms)
    {
        m_initialRetryMs = ms;
        m_retryMs = ms;
    }
#endif

Q_SIGNALS:
    void statusChanged(RelayConnection::Status status);
    void tunnelReady(const QString &requestId, const QString &token, const QString &purpose, const QString &peerFingerprint);
    void requestFailed(const QString &requestId, const QString &code);
    void incoming(const QString &token, const QString &peerFingerprint, const QString &peerKey);
    void peerOnline(const QString &peerFingerprint, const QString &peerKey);
    // Errors not tied to a request, e.g. "replaced"
    void serverError(const QString &code);

private:
    void connectNow();
    void setStatus(Status status);
    void socketConnected();
    void socketReadyRead();
    void handleMessage(const QJsonObject &message);
    void socketGone();
    void send(const QJsonObject &message);

    const QSslCertificate m_certificate;
    const QSslKey m_privateKey;
    QString m_host;
    quint16 m_port = RelayProtocol::DefaultPort;
    bool m_running = false;
    Status m_status = Status::Disabled;
    QString m_key;
    QPointer<QSslSocket> m_socket;
    bool m_tlsStarted = false;
    QTimer m_retryTimer;
    QTimer m_registrationTimer;
    QTimer m_pingTimer;
    QElapsedTimer m_lastReceived;
    int m_initialRetryMs = 5000;
    int m_retryMs = 5000;
    quint64 m_nextRequestId = 1;
    QStringList m_watchList;
};

#endif
