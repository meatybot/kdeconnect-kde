/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "relayconnection.h"

#include <QJsonArray>
#include <QNetworkProxy>
#include <QSslConfiguration>

#include "core_debug.h"

static const int REGISTRATION_TIMEOUT_MS = 15000;
static const int PING_INTERVAL_MS = 60000;
static const int MAX_RETRY_MS = 5 * 60 * 1000;

RelayConnection::RelayConnection(const QSslCertificate &certificate, const QSslKey &privateKey, QObject *parent)
    : QObject(parent)
    , m_certificate(certificate)
    , m_privateKey(privateKey)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &RelayConnection::connectNow);

    m_registrationTimer.setSingleShot(true);
    m_registrationTimer.setInterval(REGISTRATION_TIMEOUT_MS);
    connect(&m_registrationTimer, &QTimer::timeout, this, [this]() {
        qCWarning(KDECONNECT_CORE) << "Relay: registration timed out";
        if (m_socket) {
            m_socket->abort();
        }
    });

    m_pingTimer.setInterval(PING_INTERVAL_MS);
    connect(&m_pingTimer, &QTimer::timeout, this, [this]() {
        // The server drops us after 180s without traffic, detect a dead connection on our side too
        if (m_lastReceived.isValid() && m_lastReceived.elapsed() > 3 * PING_INTERVAL_MS) {
            qCWarning(KDECONNECT_CORE) << "Relay: no reply from server, reconnecting";
            m_socket->abort();
            return;
        }
        send({{QStringLiteral("type"), QStringLiteral("ping")}});
    });
}

RelayConnection::~RelayConnection()
{
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        delete m_socket;
    }
}

QString RelayConnection::statusString(Status status)
{
    switch (status) {
    case Status::Disabled:
        return QStringLiteral("disabled");
    case Status::Connecting:
        return QStringLiteral("connecting");
    case Status::Registered:
        return QStringLiteral("registered");
    case Status::Offline:
        return QStringLiteral("offline");
    }
    return QString();
}

void RelayConnection::setServer(const QString &host, quint16 port)
{
    if (m_host == host && m_port == port) {
        return;
    }
    m_host = host;
    m_port = port;
    if (m_running) {
        stop();
        start();
    }
}

void RelayConnection::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_retryMs = m_initialRetryMs;
    connectNow();
}

void RelayConnection::stop()
{
    m_running = false;
    m_retryTimer.stop();
    m_registrationTimer.stop();
    m_pingTimer.stop();
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_key.clear();
    setStatus(Status::Disabled);
}

void RelayConnection::retryNow()
{
    if (m_running && m_status == Status::Offline) {
        m_retryTimer.stop();
        m_retryMs = m_initialRetryMs;
        connectNow();
    }
}

void RelayConnection::setStatus(Status status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged(status);
    }
}

void RelayConnection::connectNow()
{
    if (!m_running) {
        return;
    }
    if (m_host.isEmpty()) {
        setStatus(Status::Offline);
        return;
    }

    m_tlsStarted = false;
    m_key.clear();
    m_socket = new QSslSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(m_certificate);
    config.setPrivateKey(m_privateKey);
    // The relay is not trusted with anything but metadata, its certificate is not verified (see spec)
    config.setPeerVerifyMode(QSslSocket::QueryPeer);
    m_socket->setSslConfiguration(config);

    connect(m_socket, &QSslSocket::sslErrors, m_socket, [socket = m_socket.data()](const QList<QSslError> &) {
        socket->ignoreSslErrors();
    });
    connect(m_socket, &QAbstractSocket::connected, this, &RelayConnection::socketConnected);
    connect(m_socket, &QIODevice::readyRead, this, &RelayConnection::socketReadyRead);
    connect(m_socket, &QAbstractSocket::disconnected, this, &RelayConnection::socketGone);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        qCDebug(KDECONNECT_CORE) << "Relay: socket error" << error;
        if (m_socket && m_socket->state() != QAbstractSocket::ConnectedState) {
            socketGone();
        }
    });

    setStatus(Status::Connecting);
    m_registrationTimer.start();
    m_socket->connectToHost(m_host, m_port);
}

void RelayConnection::socketConnected()
{
    send({{QStringLiteral("type"), QStringLiteral("hello")}, {QStringLiteral("version"), RelayProtocol::Version}});
}

void RelayConnection::socketReadyRead()
{
    QSslSocket *socket = m_socket;
    while (socket && socket == m_socket && socket->canReadLine()) {
        const QByteArray line = socket->readLine(RelayProtocol::MaxLineSize + 1);
        m_lastReceived.start();
        QJsonObject message;
        if (!line.endsWith('\n') || !RelayProtocol::decode(line, &message)) {
            qCWarning(KDECONNECT_CORE) << "Relay: malformed message from server";
            socket->abort();
            return;
        }
        if (!m_tlsStarted) {
            if (message.value(QLatin1String("type")).toString() != QLatin1String("starttls")) {
                socket->abort();
                return;
            }
            m_tlsStarted = true;
            socket->startClientEncryption();
            return;
        }
        handleMessage(message);
    }
    if (socket && socket == m_socket && !socket->canReadLine() && socket->bytesAvailable() > RelayProtocol::MaxLineSize) {
        socket->abort();
    }
}

void RelayConnection::handleMessage(const QJsonObject &message)
{
    const QString type = message.value(QLatin1String("type")).toString();
    auto str = [&message](const char *name) {
        return message.value(QLatin1String(name)).toString();
    };

    if (type == QLatin1String("welcome")) {
        if (message.value(QLatin1String("version")).toInt() != RelayProtocol::Version) {
            qCWarning(KDECONNECT_CORE) << "Relay: unsupported protocol version" << message.value(QLatin1String("version"));
            m_socket->abort();
            return;
        }
        const QString expectedKey = RelayProtocol::relayKey(m_certificate);
        if (str("key") != expectedKey) {
            qCWarning(KDECONNECT_CORE) << "Relay: server reported a different key than ours, ignoring it";
        }
        m_key = expectedKey;
        m_registrationTimer.stop();
        m_retryMs = m_initialRetryMs;
        m_pingTimer.start();
        setStatus(Status::Registered);
        if (!m_watchList.isEmpty()) {
            setWatchList(m_watchList);
        }
    } else if (type == QLatin1String("tunnel")) {
        Q_EMIT tunnelReady(str("requestId"), str("token"), str("purpose"), RelayProtocol::parseTarget(str("peerFingerprint")).value);
    } else if (type == QLatin1String("incoming")) {
        const RelayProtocol::Target peer = RelayProtocol::parseTarget(str("peerFingerprint"));
        if (peer.kind == RelayProtocol::Target::Fingerprint && !str("token").isEmpty()) {
            Q_EMIT incoming(str("token"), peer.value, str("peerKey"));
        }
    } else if (type == QLatin1String("peer_online")) {
        const RelayProtocol::Target peer = RelayProtocol::parseTarget(str("peerFingerprint"));
        if (peer.kind == RelayProtocol::Target::Fingerprint) {
            Q_EMIT peerOnline(peer.value, str("peerKey"));
        }
    } else if (type == QLatin1String("error")) {
        const QString requestId = str("requestId");
        qCDebug(KDECONNECT_CORE) << "Relay: server error" << str("code") << str("message");
        if (!requestId.isEmpty()) {
            Q_EMIT requestFailed(requestId, str("code"));
        } else {
            if (str("code") == QLatin1String("replaced")) {
                // Another instance uses our certificate; don't fight over the registration
                m_retryMs = MAX_RETRY_MS;
            }
            Q_EMIT serverError(str("code"));
        }
    } else if (type != QLatin1String("pong")) {
        qCDebug(KDECONNECT_CORE) << "Relay: ignoring unknown message" << type;
    }
}

void RelayConnection::socketGone()
{
    if (!m_socket) {
        return;
    }
    disconnect(m_socket, nullptr, this, nullptr);
    m_socket->deleteLater();
    m_socket = nullptr;
    m_key.clear();
    m_registrationTimer.stop();
    m_pingTimer.stop();
    m_lastReceived.invalidate();
    if (!m_running) {
        return;
    }
    setStatus(Status::Offline);
    qCDebug(KDECONNECT_CORE) << "Relay: disconnected, retrying in" << m_retryMs << "ms";
    m_retryTimer.start(m_retryMs);
    m_retryMs = qMin(m_retryMs * 2, MAX_RETRY_MS);
}

void RelayConnection::send(const QJsonObject &message)
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(RelayProtocol::encode(message));
    }
}

QString RelayConnection::requestTunnel(const RelayProtocol::Target &target, const QString &purpose)
{
    if (m_status != Status::Registered || !target.isValid()) {
        return QString();
    }
    const QString requestId = QString::number(m_nextRequestId++);
    send({
        {QStringLiteral("type"), QStringLiteral("connect")},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("target"), target.value},
        {QStringLiteral("purpose"), purpose},
    });
    return requestId;
}

void RelayConnection::setWatchList(const QStringList &fingerprints)
{
    m_watchList = fingerprints;
    if (m_status == Status::Registered) {
        send({{QStringLiteral("type"), QStringLiteral("watch")}, {QStringLiteral("fingerprints"), QJsonArray::fromStringList(fingerprints)}});
    }
}

#include "moc_relayconnection.cpp"
