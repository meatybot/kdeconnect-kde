/**
 * SPDX-FileCopyrightText: 2015 Holger Kaelberer <holger.k@elberer.de>
 * SPDX-FileCopyrightText: 2018 Richard Liebscher <richard.liebscher@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "forwardednotifications.h"
#include "notificationslistener.h"

#include "pw_libnotificationmanager/notification.h"

#include <atomic>

#include <QHash>
#include <QThread>

#include <dbus/dbus.h>

class DBusNotificationsListenerThread : public QThread
{
    Q_OBJECT

public:
    void run() override;
    void stop();
    void handleMessage(DBusMessage *message);

Q_SIGNALS:
    void notificationReceived(const NotificationManager::Notification &);
    void notificationClosed(uint id, uint reason);

private:
    struct PendingNotify {
        QString appName;
        uint replacesId;
        QString appIcon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout;
    };

    void handleNotifyCall(DBusMessage *message);
    void handleNotifyReturn(DBusMessage *message);
    void emitNotification(const PendingNotify &call, uint id);

    std::atomic<DBusConnection *> m_connection = nullptr;
    // Notify calls waiting for the server to return the notification id, by (caller, serial)
    QHash<QPair<QString, dbus_uint32_t>, PendingNotify> m_pendingNotify;
};

class DBusNotificationsListener : public NotificationsListener
{
    Q_OBJECT

public:
    explicit DBusNotificationsListener(KdeConnectPlugin *aPlugin);
    ~DBusNotificationsListener() override;

    void handlePacket(const NetworkPacket &np) override;

private:
    void onNotify(const NotificationManager::Notification &);
    void onNotificationClosed(uint id, uint reason);
    void checkServerCapabilities();
    void invokeAction(uint id, const QString &actionKey);
    void closeNotification(uint id);
    QByteArray iconForNotification(const NotificationManager::Notification &notification) const;

    DBusNotificationsListenerThread *m_thread = nullptr;
    ForwardedNotifications m_forwarded;
    // The notification server implements org.kde.NotificationManager.InvokeAction (Plasma)
    bool m_canInvokeActions = false;
};
