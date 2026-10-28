/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "relaydevicelink.h"

#include <QTimer>

#include "backends/lan/uploadjob.h"
#include "core_debug.h"
#include "relaylinkprovider.h"

// Same limit as LanDeviceLink
static const int MAX_PACKET_SIZE = 32 * 1024 * 1024;

RelayDeviceLink::RelayDeviceLink(const DeviceInfo &deviceInfo, RelayLinkProvider *parent, RelayTunnelSocket *socket)
    : DeviceLink(deviceInfo.id, parent)
    , m_provider(parent)
    , m_deviceInfo(deviceInfo)
{
    reset(socket, deviceInfo);
}

void RelayDeviceLink::reset(RelayTunnelSocket *socket, const DeviceInfo &deviceInfo)
{
    m_deviceInfo = deviceInfo;

    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        delete m_socket;
    }
    m_socket = socket;
    socket->setParent(this);

    connect(socket, &QAbstractSocket::disconnected, this, [this, socket]() {
        QTimer::singleShot(0, this, [this, socket]() {
            if (m_socket == socket) {
                delete this;
            }
        });
    });
    connect(socket, &QIODevice::readyRead, this, &RelayDeviceLink::dataReceived);
    if (socket->bytesAvailable()) {
        QMetaObject::invokeMethod(this, &RelayDeviceLink::dataReceived, Qt::QueuedConnection);
    }
}

QString RelayDeviceLink::address() const
{
    return m_provider->server();
}

bool RelayDeviceLink::sendPacket(NetworkPacket &np)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    if (np.payload() && np.hasPayload()) {
        PendingUpload upload{np, QString()};
        if (np.payloadSize() < 0) {
            requestPayloadTunnel(upload);
            if (upload.requestId.isEmpty()) {
                return false;
            }
            m_endless.append(upload);
        } else {
            m_finiteQueue.enqueue(upload);
            startNextUpload();
        }
        return true;
    }

    return m_socket->write(np.serialize()) != -1;
}

void RelayDeviceLink::requestPayloadTunnel(PendingUpload &upload)
{
    upload.requestId = m_provider->requestPayloadTunnel(this);
    if (upload.requestId.isEmpty()) {
        qCWarning(KDECONNECT_CORE) << "Relay: cannot request a payload tunnel, not registered";
    }
}

void RelayDeviceLink::startNextUpload()
{
    while (!m_finiteBusy && !m_finiteQueue.isEmpty()) {
        PendingUpload &next = m_finiteQueue.head();
        requestPayloadTunnel(next);
        if (!next.requestId.isEmpty()) {
            m_finiteBusy = true;
            return;
        }
        PendingUpload dropped = m_finiteQueue.dequeue();
        if (dropped.packet.payload()) {
            dropped.packet.payload()->close();
        }
    }
}

void RelayDeviceLink::payloadTunnelReady(const QString &requestId, const QString &token)
{
    if (m_finiteBusy && !m_finiteQueue.isEmpty() && m_finiteQueue.head().requestId == requestId) {
        const PendingUpload upload = m_finiteQueue.dequeue();
        beginUpload(upload.packet, token);
        return;
    }
    for (int i = 0; i < m_endless.size(); ++i) {
        if (m_endless[i].requestId == requestId) {
            const PendingUpload upload = m_endless.takeAt(i);
            beginUpload(upload.packet, token);
            return;
        }
    }
}

void RelayDeviceLink::payloadTunnelFailed(const QString &requestId)
{
    qCWarning(KDECONNECT_CORE) << "Relay: payload tunnel request failed";
    if (m_finiteBusy && !m_finiteQueue.isEmpty() && m_finiteQueue.head().requestId == requestId) {
        PendingUpload upload = m_finiteQueue.dequeue();
        if (upload.packet.payload()) {
            upload.packet.payload()->close();
        }
        m_finiteBusy = false;
        startNextUpload();
        return;
    }
    for (int i = 0; i < m_endless.size(); ++i) {
        if (m_endless[i].requestId == requestId) {
            PendingUpload upload = m_endless.takeAt(i);
            if (upload.packet.payload()) {
                upload.packet.payload()->close();
            }
            return;
        }
    }
}

void RelayDeviceLink::beginUpload(const NetworkPacket &packet, const QString &token)
{
    const bool finite = packet.payloadSize() >= 0;

    // The receiver attaches with the token from the header packet and becomes the TLS client
    RelayTunnelSocket *socket =
        m_provider->openTunnel(token, RelayTunnelSocket::Role::TlsServer, RelayProtocol::targetForCertificate(m_deviceInfo.certificate), nullptr);
    UploadJob *job = new UploadJob(packet);
    job->setSocket(socket); // The job owns the socket from now on

    auto done = [self = QPointer<RelayDeviceLink>(this), finite]() {
        if (finite && self) {
            self->m_finiteBusy = false;
            self->startNextUpload();
        }
    };
    connect(job, &KJob::result, this, done);
    connect(socket, &RelayTunnelSocket::established, job, &UploadJob::start);
    connect(socket, &RelayTunnelSocket::failed, job, [job, done]() {
        qCWarning(KDECONNECT_CORE) << "Relay: payload tunnel failed";
        // Not started yet, so no result will come: release the payload and the job ourselves
        job->stop();
        job->deleteLater();
        done();
    });
    // The receiver closing the stream early (e.g. cancelled) ends the upload
    connect(socket, &QAbstractSocket::disconnected, job, [job]() {
        job->stop();
    });

    // The header doesn't carry the payload device, only the token to fetch it
    NetworkPacket header = packet;
    header.setPayload(nullptr, packet.payloadSize());
    header.setPayloadTransferInfo({{QStringLiteral("relayToken"), token}});
    m_socket->write(header.serialize());
}

void RelayDeviceLink::dataReceived()
{
    if (!m_socket->canReadLine() && m_socket->bytesAvailable() > MAX_PACKET_SIZE) {
        qCWarning(KDECONNECT_CORE) << "Relay: discarding packet that's too large";
        m_socket->readAll();
    }
    while (m_socket->canReadLine()) {
        const QByteArray serializedPacket = m_socket->readLine();
        NetworkPacket packet;
        if (!NetworkPacket::unserialize(serializedPacket, &packet)) {
            continue;
        }

        if (packet.hasPayloadTransferInfo()) {
            const QString token = packet.payloadTransferInfo().value(QStringLiteral("relayToken")).toString();
            if (token.isEmpty()) {
                qCWarning(KDECONNECT_CORE) << "Relay: payload packet without relay token, ignoring payload";
                packet.setPayloadTransferInfo({});
            } else {
                QSharedPointer<QSslSocket> socket(
                    m_provider->openTunnel(token, RelayTunnelSocket::Role::TlsClient, RelayProtocol::targetForCertificate(m_deviceInfo.certificate), nullptr));
                // Same workaround as LanDeviceLink, QTBUG-62257
                connect(socket.data(), &QAbstractSocket::disconnected, socket.data(), &QAbstractSocket::readChannelFinished);
                packet.setPayload(socket, packet.payloadSize());
            }
        }

        Q_EMIT receivedPacket(packet);
    }
}

#include "moc_relaydevicelink.cpp"
