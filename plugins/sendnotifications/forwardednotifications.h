/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <core/networkpacket.h>

#define PACKET_TYPE_NOTIFICATION QStringLiteral("kdeconnect.notification")
#define PACKET_TYPE_NOTIFICATION_REQUEST QStringLiteral("kdeconnect.notification.request")
#define PACKET_TYPE_NOTIFICATION_ACTION QStringLiteral("kdeconnect.notification.action")

// A local notification that was forwarded to the peer, as far as the peer may interact with it
struct ForwardedNotification {
    uint id = 0;
    QString appName;
    QString title;
    QString body; // Empty if the body must not be sent
    QStringList actionIds;
    QStringList actionLabels;
    bool hasDefaultAction = false;
    bool resident = false; // The app keeps the notification after an action was invoked
    int urgency = -1; // -1: not set, otherwise 0 (low), 1 (normal), 2 (critical)
    int progress = -1; // -1: not set, otherwise 0-100
    QString category;
    bool clearable = true;
    qint64 time = 0; // ms since epoch
    QByteArray icon; // PNG, may be empty

    QString ticker() const;
    QString iconHash() const;
};

/**
 * Keeps track of the notifications forwarded to one peer, so that actions and
 * dismissals coming back from it can be mapped to the local notification server
 * and limited to notifications we actually sent.
 */
class ForwardedNotifications
{
public:
    static constexpr int s_maxTracked = 100;

    // Returns the previous entry with the same id, if any
    std::optional<ForwardedNotification> insert(const ForwardedNotification &notification);
    bool remove(uint id);
    const ForwardedNotification *find(uint id) const;
    QList<uint> ids() const;

    // Maps an action from the peer to a local action id, empty if not found.
    // actionId (new peers) takes precedence over the label (all peers).
    QString resolveAction(uint id, const QString &actionId, const QString &label) const;

    static std::optional<uint> parseKey(const QString &key);
    static NetworkPacket toPacket(const ForwardedNotification &notification, bool silent, bool withIcon);
    static NetworkPacket cancelPacket(uint id);

private:
    QHash<uint, ForwardedNotification> m_notifications;
    QList<uint> m_order; // Oldest first
};
