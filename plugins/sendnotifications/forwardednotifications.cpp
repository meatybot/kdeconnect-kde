/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "forwardednotifications.h"

#include <QBuffer>
#include <QCryptographicHash>

QString ForwardedNotification::ticker() const
{
    if (body.isEmpty()) {
        return title;
    }
    return title + QLatin1String(": ") + body;
}

QString ForwardedNotification::iconHash() const
{
    if (icon.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(QCryptographicHash::hash(icon, QCryptographicHash::Md5).toHex());
}

std::optional<ForwardedNotification> ForwardedNotifications::insert(const ForwardedNotification &notification)
{
    std::optional<ForwardedNotification> previous;
    auto it = m_notifications.find(notification.id);
    if (it != m_notifications.end()) {
        previous = *it;
        *it = notification;
        m_order.removeOne(notification.id);
    } else {
        m_notifications.insert(notification.id, notification);
    }
    m_order.append(notification.id);

    while (m_order.size() > s_maxTracked) {
        m_notifications.remove(m_order.takeFirst());
    }
    return previous;
}

bool ForwardedNotifications::remove(uint id)
{
    m_order.removeOne(id);
    return m_notifications.remove(id) > 0;
}

const ForwardedNotification *ForwardedNotifications::find(uint id) const
{
    auto it = m_notifications.constFind(id);
    return it == m_notifications.cend() ? nullptr : &*it;
}

QList<uint> ForwardedNotifications::ids() const
{
    return m_order;
}

QString ForwardedNotifications::resolveAction(uint id, const QString &actionId, const QString &label) const
{
    const ForwardedNotification *notification = find(id);
    if (!notification) {
        return QString();
    }

    if (!actionId.isEmpty()) {
        if (actionId == QLatin1String("default")) {
            return notification->hasDefaultAction ? actionId : QString();
        }
        return notification->actionIds.contains(actionId) ? actionId : QString();
    }

    // Legacy peers only know the label
    const qsizetype index = notification->actionLabels.indexOf(label);
    return index < 0 ? QString() : notification->actionIds.at(index);
}

std::optional<uint> ForwardedNotifications::parseKey(const QString &key)
{
    bool ok = false;
    const uint id = key.toUInt(&ok);
    if (!ok || id == 0) {
        return std::nullopt;
    }
    return id;
}

NetworkPacket ForwardedNotifications::toPacket(const ForwardedNotification &notification, bool silent, bool withIcon)
{
    NetworkPacket np(PACKET_TYPE_NOTIFICATION,
                     {
                         {QStringLiteral("id"), QString::number(notification.id)},
                         {QStringLiteral("appName"), notification.appName},
                         {QStringLiteral("ticker"), notification.ticker()},
                         {QStringLiteral("isClearable"), notification.clearable},
                         {QStringLiteral("title"), notification.title},
                         {QStringLiteral("silent"), silent},
                         {QStringLiteral("time"), QString::number(notification.time)},
                     });

    if (!notification.body.isEmpty()) {
        np.set(QStringLiteral("text"), notification.body);
    }

    if (!notification.actionIds.isEmpty()) {
        // "actions" holds the labels, as sent by kdeconnect-android, so that older peers keep working
        np.set(QStringLiteral("actions"), notification.actionLabels);
        np.set(QStringLiteral("actionIds"), notification.actionIds);
    }
    if (notification.hasDefaultAction) {
        np.set(QStringLiteral("hasDefaultAction"), true);
    }
    if (notification.resident) {
        np.set(QStringLiteral("resident"), true);
    }
    if (notification.urgency >= 0) {
        np.set(QStringLiteral("urgency"), notification.urgency);
    }
    if (notification.progress >= 0) {
        np.set(QStringLiteral("progress"), notification.progress);
    }
    if (!notification.category.isEmpty()) {
        np.set(QStringLiteral("category"), notification.category);
    }

    if (!notification.icon.isEmpty()) {
        // The hash is always sent, so the peer can reuse an icon it already has
        np.set(QStringLiteral("payloadHash"), notification.iconHash());
        if (withIcon) {
            auto buffer = QSharedPointer<QBuffer>::create();
            buffer->setData(notification.icon);
            np.setPayload(buffer, notification.icon.size());
        }
    }

    return np;
}

NetworkPacket ForwardedNotifications::cancelPacket(uint id)
{
    return NetworkPacket(PACKET_TYPE_NOTIFICATION,
                         {
                             {QStringLiteral("id"), QString::number(id)},
                             {QStringLiteral("isCancel"), true},
                         });
}
