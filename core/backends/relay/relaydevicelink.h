/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#ifndef RELAYDEVICELINK_H
#define RELAYDEVICELINK_H

#include <QHash>
#include <QPointer>
#include <QQueue>

#include "backends/devicelink.h"
#include "deviceinfo.h"
#include "kdeconnectcore_export.h"
#include "relaytunnelsocket.h"

class RelayLinkProvider;
class UploadJob;

// A device link over an end-to-end TLS session spliced by a relay server. Payloads use extra relay tunnels.
class KDECONNECTCORE_EXPORT RelayDeviceLink : public DeviceLink
{
    Q_OBJECT

public:
    RelayDeviceLink(const DeviceInfo &deviceInfo, RelayLinkProvider *parent, RelayTunnelSocket *socket);

    void reset(RelayTunnelSocket *socket, const DeviceInfo &deviceInfo);
    bool sendPacket(NetworkPacket &np) override;
    DeviceInfo deviceInfo() const override
    {
        return m_deviceInfo;
    }
    QString address() const override;

    // Called by the provider when the relay answers a payload tunnel request made by this link
    void payloadTunnelReady(const QString &requestId, const QString &token);
    void payloadTunnelFailed(const QString &requestId);

private:
    struct PendingUpload {
        NetworkPacket packet;
        QString requestId;
    };

    void dataReceived();
    void startNextUpload();
    void requestPayloadTunnel(PendingUpload &upload);
    void beginUpload(const NetworkPacket &header, const QString &token);

    RelayLinkProvider *m_provider;
    RelayTunnelSocket *m_socket = nullptr;
    DeviceInfo m_deviceInfo;

    // Finite payloads are sent one at a time, endless ones (payloadSize -1) right away
    QQueue<PendingUpload> m_finiteQueue;
    bool m_finiteBusy = false;
    QList<PendingUpload> m_endless;
};

#endif
