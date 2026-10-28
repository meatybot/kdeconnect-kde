/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "relaylinkprovider.h"

#include "core_debug.h"
#include "dbushelper.h"
#include "kdeconnectconfig.h"

static const int MAX_IDENTITY_PACKET_SIZE = 8192;
static const int IDENTITY_TIMEOUT_MS = 10000;
static const int MAX_UNPAIRED_CONNECTIONS = 42;
static const int MAX_PENDING_TUNNELS = 8;

RelayLinkProvider::RelayLinkProvider(bool disabled)
    : m_connection(KdeConnectConfig::instance().certificate(), KdeConnectConfig::instance().privateKey(), this)
    , m_disabled(disabled)
{
    connect(&m_connection, &RelayConnection::statusChanged, this, [this](RelayConnection::Status status) {
        qCDebug(KDECONNECT_CORE) << "Relay status:" << RelayConnection::statusString(status);
        if (status == RelayConnection::Status::Registered) {
            refreshWatchList();
        } else {
            m_pendingLinkRequests.clear();
            const auto pendingPayloads = m_pendingPayloadRequests;
            m_pendingPayloadRequests.clear();
            for (auto it = pendingPayloads.cbegin(); it != pendingPayloads.cend(); ++it) {
                if (it.value()) {
                    it.value()->payloadTunnelFailed(it.key());
                }
            }
        }
        Q_EMIT statusChanged();
    });
    connect(&m_connection, &RelayConnection::tunnelReady, this, &RelayLinkProvider::tunnelReady);
    connect(&m_connection, &RelayConnection::requestFailed, this, &RelayLinkProvider::requestFailed);
    connect(&m_connection, &RelayConnection::incoming, this, [this](const QString &token, const QString &peerFingerprint) {
        // The peer pins us; we pin the certificate the relay says it belongs to (and the stored one if paired)
        startLinkTunnel(token, RelayTunnelSocket::Role::TlsClient, {RelayProtocol::Target::Fingerprint, peerFingerprint});
    });
    connect(&m_connection, &RelayConnection::peerOnline, this, [this](const QString &peerFingerprint) {
        if (hasLinkWithFingerprint(peerFingerprint)) {
            return;
        }
        // Only one side initiates, so both don't open a tunnel at the same time
        if (RelayProtocol::fingerprint(KdeConnectConfig::instance().certificate()) < peerFingerprint) {
            connectTo({RelayProtocol::Target::Fingerprint, peerFingerprint});
        }
    });
}

RelayLinkProvider::~RelayLinkProvider()
{
    m_connection.stop();
}

void RelayLinkProvider::applyConfig()
{
    QString host;
    quint16 port = RelayProtocol::DefaultPort;
    if (!RelayProtocol::parseAddress(KdeConnectConfig::instance().relayServer(), &host, &port)) {
        host.clear();
    }
    m_connection.setServer(host, port);
    if (m_started && !m_disabled && !host.isEmpty()) {
        m_connection.start();
    } else {
        m_connection.stop();
    }
}

void RelayLinkProvider::enable()
{
    if (m_disabled) {
        m_disabled = false;
        onStart();
    }
}

void RelayLinkProvider::disable()
{
    if (!m_disabled) {
        onStop();
        m_disabled = true;
    }
}

void RelayLinkProvider::onStart()
{
    if (m_disabled) {
        return;
    }
    m_started = true;
    applyConfig();
}

void RelayLinkProvider::onStop()
{
    m_started = false;
    m_connection.stop();
    const auto links = m_links.values();
    qDeleteAll(links); // Removed from m_links by onLinkDestroyed()
}

void RelayLinkProvider::onNetworkChange()
{
    m_connection.retryNow();
}

bool RelayLinkProvider::setServer(const QString &address)
{
    const QString trimmed = address.trimmed();
    QString host;
    quint16 port;
    if (!trimmed.isEmpty() && !RelayProtocol::parseAddress(trimmed, &host, &port)) {
        return false;
    }
    KdeConnectConfig::instance().setRelayServer(trimmed);
    applyConfig();
    return true;
}

QString RelayLinkProvider::server() const
{
    return KdeConnectConfig::instance().relayServer();
}

bool RelayLinkProvider::connectTo(const RelayProtocol::Target &target)
{
    if (!target.isValid() || target == RelayProtocol::targetForCertificate(KdeConnectConfig::instance().certificate())
        || target.value == m_connection.key()) {
        return false;
    }
    const QString requestId = m_connection.requestTunnel(target, QStringLiteral("link"));
    if (requestId.isEmpty()) {
        return false;
    }
    m_pendingLinkRequests.insert(requestId, target);
    return true;
}

void RelayLinkProvider::refreshWatchList()
{
    QStringList fingerprints;
    const QStringList trusted = KdeConnectConfig::instance().trustedDevices();
    for (const QString &id : trusted) {
        const QString fingerprint = RelayProtocol::fingerprint(KdeConnectConfig::instance().getTrustedDeviceCertificate(id));
        if (!fingerprint.isEmpty()) {
            fingerprints.append(fingerprint);
        }
    }
    m_connection.setWatchList(fingerprints);
}

QString RelayLinkProvider::requestPayloadTunnel(RelayDeviceLink *link)
{
    const QString requestId = m_connection.requestTunnel(RelayProtocol::targetForCertificate(link->deviceInfo().certificate), QStringLiteral("payload"));
    if (!requestId.isEmpty()) {
        m_pendingPayloadRequests.insert(requestId, link);
    }
    return requestId;
}

RelayTunnelSocket *RelayLinkProvider::openTunnel(const QString &token, RelayTunnelSocket::Role role, const RelayProtocol::Target &pin, QObject *parent)
{
    RelayTunnelSocket *socket = new RelayTunnelSocket(parent);
    socket->open(m_connection.host(), m_connection.port(), token, role, m_connection.certificate(), m_connection.privateKey(), pin);
    return socket;
}

void RelayLinkProvider::tunnelReady(const QString &requestId, const QString &token, const QString &purpose, const QString &peerFingerprint)
{
    Q_UNUSED(peerFingerprint); // Never trusted: the pin comes from what we asked for
    if (purpose == QLatin1String("link")) {
        const auto it = m_pendingLinkRequests.constFind(requestId);
        if (it == m_pendingLinkRequests.cend()) {
            return;
        }
        const RelayProtocol::Target pin = it.value();
        m_pendingLinkRequests.erase(it);
        startLinkTunnel(token, RelayTunnelSocket::Role::TlsServer, pin);
    } else if (purpose == QLatin1String("payload")) {
        QPointer<RelayDeviceLink> link = m_pendingPayloadRequests.take(requestId);
        if (link) {
            link->payloadTunnelReady(requestId, token);
        }
    }
}

void RelayLinkProvider::requestFailed(const QString &requestId, const QString &code)
{
    if (m_pendingLinkRequests.remove(requestId)) {
        qCInfo(KDECONNECT_CORE) << "Relay: connect request failed:" << code;
        return;
    }
    QPointer<RelayDeviceLink> link = m_pendingPayloadRequests.take(requestId);
    if (link) {
        link->payloadTunnelFailed(requestId);
    }
}

void RelayLinkProvider::startLinkTunnel(const QString &token, RelayTunnelSocket::Role role, const RelayProtocol::Target &pin)
{
    if (m_pendingTunnels.size() >= MAX_PENDING_TUNNELS) {
        qCWarning(KDECONNECT_CORE) << "Relay: too many pending tunnels, ignoring";
        return;
    }
    RelayTunnelSocket *socket = openTunnel(token, role, pin, this);
    m_pendingTunnels.insert(socket);
    connect(socket, &QObject::destroyed, this, [this, socket]() {
        m_pendingTunnels.remove(socket);
    });
    connect(socket, &RelayTunnelSocket::failed, socket, &QObject::deleteLater);
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &RelayTunnelSocket::established, this, [this, socket]() {
        m_pendingTunnels.remove(socket);
        exchangeIdentity(socket);
    });
}

void RelayLinkProvider::exchangeIdentity(RelayTunnelSocket *socket)
{
    QTimer *timer = new QTimer(socket);
    timer->setObjectName(QStringLiteral("identityTimer"));
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, socket, [socket]() {
        qCWarning(KDECONNECT_CORE) << "Relay: peer did not send its identity in time";
        socket->abort();
    });
    timer->start(IDENTITY_TIMEOUT_MS);

    connect(socket, &QIODevice::readyRead, this, [this, socket]() {
        identityReceived(socket);
    });

    NetworkPacket myIdentity = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    socket->write(myIdentity.serialize());
    if (socket->canReadLine()) {
        identityReceived(socket);
    }
}

void RelayLinkProvider::identityReceived(RelayTunnelSocket *socket)
{
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > MAX_IDENTITY_PACKET_SIZE) {
            qCWarning(KDECONNECT_CORE) << "Relay: identity packet too large";
            socket->abort();
        }
        return;
    }
    disconnect(socket, &QIODevice::readyRead, this, nullptr);
    delete socket->findChild<QTimer *>(QStringLiteral("identityTimer"));

    const QByteArray line = socket->readLine(MAX_IDENTITY_PACKET_SIZE + 1);
    NetworkPacket identity;
    if (!NetworkPacket::unserialize(line, &identity) || !DeviceInfo::isValidIdentityPacket(&identity)) {
        qCWarning(KDECONNECT_CORE) << "Relay: invalid identity packet";
        socket->abort();
        return;
    }

    const QSslCertificate certificate = socket->peerCertificate();
    QString certDeviceId = certificate.subjectDisplayName();
    DBusHelper::filterNonExportableCharacters(certDeviceId);
    const QString deviceId = identity.get<QString>(QStringLiteral("deviceId"));
    if (deviceId != certDeviceId || deviceId == KdeConnectConfig::instance().deviceId()) {
        qCWarning(KDECONNECT_CORE) << "Relay: device id in identity doesn't match the certificate" << deviceId << certDeviceId;
        socket->abort();
        return;
    }

    const int protocolVersion = identity.get<int>(QStringLiteral("protocolVersion"), 0);
    if (protocolVersion < 8) {
        qCWarning(KDECONNECT_CORE) << "Relay: ignoring device using too old protocol version" << protocolVersion;
        socket->abort();
        return;
    }

    if (KdeConnectConfig::instance().trustedDevices().contains(deviceId)) {
        if (KdeConnectConfig::instance().getTrustedDeviceCertificate(deviceId) != certificate) {
            qCWarning(KDECONNECT_CORE) << "Relay: paired device presented a different certificate, rejecting" << deviceId;
            socket->abort();
            return;
        }
        if (KdeConnectConfig::instance().getTrustedDeviceProtocolVersion(deviceId) > protocolVersion) {
            qCWarning(KDECONNECT_CORE) << "Relay: refusing protocol downgrade from" << deviceId;
            socket->abort();
            return;
        }
    }

    addLink(socket, DeviceInfo::FromIdentityPacketAndCert(identity, certificate));
}

void RelayLinkProvider::addLink(RelayTunnelSocket *socket, const DeviceInfo &deviceInfo)
{
    // From now on the link owns the socket and handles disconnection
    disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    disconnect(socket, &RelayTunnelSocket::failed, socket, &QObject::deleteLater);

    auto it = m_links.find(deviceInfo.id);
    if (it != m_links.end()) {
        if (it.value()->deviceInfo().certificate != deviceInfo.certificate) {
            qCWarning(KDECONNECT_CORE) << "Relay: link replacement with a different certificate, aborting";
            socket->abort();
            socket->deleteLater();
            return;
        }
        it.value()->reset(socket, deviceInfo);
        Q_EMIT deviceInfoUpdated(deviceInfo);
        return;
    }

    if (!KdeConnectConfig::instance().trustedDevices().contains(deviceInfo.id) && m_links.size() >= MAX_UNPAIRED_CONNECTIONS) {
        qCWarning(KDECONNECT_CORE) << "Relay: too many devices, ignoring" << deviceInfo.id;
        socket->abort();
        socket->deleteLater();
        return;
    }

    RelayDeviceLink *link = new RelayDeviceLink(deviceInfo, this, socket);
    m_links.insert(deviceInfo.id, link);
    Q_EMIT connectionReceived(link);
}

bool RelayLinkProvider::hasLinkWithFingerprint(const QString &fingerprint) const
{
    for (const RelayDeviceLink *link : m_links) {
        if (RelayProtocol::fingerprint(link->deviceInfo().certificate) == fingerprint) {
            return true;
        }
    }
    return false;
}

void RelayLinkProvider::onLinkDestroyed(const QString &deviceId, DeviceLink *oldPtr)
{
    if (m_links.value(deviceId) == oldPtr) {
        m_links.remove(deviceId);
        // If both are still registered, the relay tells us again and we reconnect
        if (m_connection.status() == RelayConnection::Status::Registered && KdeConnectConfig::instance().trustedDevices().contains(deviceId)) {
            refreshWatchList();
        }
    }
}

#include "moc_relaylinkprovider.cpp"
