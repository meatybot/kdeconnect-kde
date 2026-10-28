/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef CONNECTTARGETS_H
#define CONNECTTARGETS_H

#include <QElapsedTimer>
#include <QList>

#include "relayprotocol.h"
#include "kdeconnectcore_export.h"

// Devices the user asked to connect to by relay key or certificate fingerprint. When a link whose certificate
// matches appears (on any backend) the target is consumed and pairing gets requested.
class KDECONNECTCORE_EXPORT ConnectTargets
{
public:
    static const qint64 EXPIRY_MS = 5 * 60 * 1000;

    // Returns false if the target is invalid
    bool add(const RelayProtocol::Target &target);
    // Removes and returns true if a non-expired target matches the certificate
    bool take(const QSslCertificate &certificate);
    bool isEmpty();

#ifdef BUILD_TESTING
    void testSetExpiry(qint64 ms)
    {
        m_expiryMs = ms;
    }
#endif

private:
    void prune();

    struct Entry {
        RelayProtocol::Target target;
        QElapsedTimer added;
    };
    QList<Entry> m_entries;
    qint64 m_expiryMs = EXPIRY_MS;
};

#endif
