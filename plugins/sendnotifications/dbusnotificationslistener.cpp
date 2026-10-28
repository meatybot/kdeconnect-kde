/**
 * SPDX-FileCopyrightText: 2015 Holger Kaelberer <holger.k@elberer.de>
 * SPDX-FileCopyrightText: 2018 Richard Liebscher <richard.liebscher@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dbusnotificationslistener.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include <QBuffer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QImage>

#include <kiconloader.h>
#include <kicontheme.h>

#include "plugin_sendnotifications_debug.h"
#include <core/kdeconnectplugin.h>
#include <core/kdeconnectpluginconfig.h>

namespace
{
// https://specifications.freedesktop.org/notification-spec/notification-spec-latest.html
inline constexpr const char *NOTIFY_SIGNATURE = "susssasa{sv}i";
// inline constexpr const char *IMAGE_DATA_SIGNATURE = "iiibiiay";

QString becomeMonitor(DBusConnection *conn, const char **matches, int count)
{
    // message
    DBusMessage *msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_MONITORING, "BecomeMonitor");
    Q_ASSERT(msg != nullptr);

    // arguments
    dbus_uint32_t flags = 0;

    bool success = dbus_message_append_args(msg, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &matches, count, DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID);
    if (!success) {
        dbus_message_unref(msg);
        return QStringLiteral("Failed to call dbus_message_append_args");
    }

    // send
    // TODO: wait and check for error: dbus_connection_send_with_reply_and_block
    success = dbus_connection_send(conn, msg, nullptr);
    if (!success) {
        dbus_message_unref(msg);
        return QStringLiteral("Failed to call dbus_connection_send");
    }

    dbus_message_unref(msg);

    return QString();
}

extern "C" DBusHandlerResult handleMessageFromC(DBusConnection *, DBusMessage *message, void *user_data)
{
    auto *self = static_cast<DBusNotificationsListenerThread *>(user_data);
    self->handleMessage(message);
    // Monitors must not allow libdbus to reply to messages, so we eat the message.
    return DBUS_HANDLER_RESULT_HANDLED;
}

unsigned nextUnsigned(DBusMessageIter *iter)
{
    Q_ASSERT(dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_UINT32);
    DBusBasicValue value;
    dbus_message_iter_get_basic(iter, &value);
    dbus_message_iter_next(iter);
    return value.u32;
}

int nextInt(DBusMessageIter *iter)
{
    Q_ASSERT(dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_INT32);
    DBusBasicValue value;
    dbus_message_iter_get_basic(iter, &value);
    dbus_message_iter_next(iter);
    return value.i32;
}

QString nextString(DBusMessageIter *iter)
{
    Q_ASSERT(dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_STRING);
    DBusBasicValue value;
    dbus_message_iter_get_basic(iter, &value);
    dbus_message_iter_next(iter);
    return QString::fromUtf8(value.str);
}

QStringList nextStringList(DBusMessageIter *iter)
{
    DBusMessageIter sub;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_next(iter);

    QStringList list;
    while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        list.append(nextString(&sub));
    }
    return list;
}

QVariant nextVariant(DBusMessageIter *iter)
{
    int type = dbus_message_iter_get_arg_type(iter);
    if (type != DBUS_TYPE_VARIANT) {
        return QVariant();
    }

    DBusMessageIter sub;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_next(iter);

    type = dbus_message_iter_get_arg_type(&sub);
    if (dbus_type_is_basic(type)) {
        DBusBasicValue value;
        dbus_message_iter_get_basic(&sub, &value);
        switch (type) {
        case DBUS_TYPE_BOOLEAN:
            return QVariant(value.bool_val);
        case DBUS_TYPE_INT16:
            return QVariant(value.i16);
        case DBUS_TYPE_INT32:
            return QVariant(value.i32);
        case DBUS_TYPE_INT64:
            return QVariant((qlonglong)value.i64);
        case DBUS_TYPE_UINT16:
            return QVariant(value.u16);
        case DBUS_TYPE_UINT32:
            return QVariant(value.u32);
        case DBUS_TYPE_UINT64:
            return QVariant((qulonglong)value.u64);
        case DBUS_TYPE_BYTE:
            return QVariant(value.byt);
        case DBUS_TYPE_DOUBLE:
            return QVariant(value.dbl);
        case DBUS_TYPE_STRING:
            return QVariant(QString::fromUtf8(value.str));
        case DBUS_STRUCT_BEGIN_CHAR: {
        }
        default:
            break;
        }
    }

    qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Unimplemented conversation of type" << QChar(type) << type;

    return QVariant();
}

static QVariantMap nextVariantMap(DBusMessageIter *iter)
{
    DBusMessageIter sub;
    dbus_message_iter_recurse(iter, &sub);
    dbus_message_iter_next(iter);

    QVariantMap map;
    while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&sub, &entry);
        dbus_message_iter_next(&sub);
        QString key = nextString(&entry);
        QVariant value = nextVariant(&entry);
        map.insert(key, value);
    }
    return map;
}
}

void DBusNotificationsListenerThread::run()
{
    DBusError err = DBUS_ERROR_INIT;
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "D-Bus connection failed" << err.message;
        dbus_error_free(&err);
        return;
    }

    Q_ASSERT(m_connection != nullptr);

    dbus_connection_set_route_peer_messages(m_connection, true);
    dbus_connection_set_exit_on_disconnect(m_connection, false);
    dbus_connection_add_filter(m_connection, handleMessageFromC, this, nullptr);

    // Notify calls, the replies carrying the id assigned by the server, and closed notifications.
    // The bus matches a well-known sender name against its current owner.
    const char *matches[] = {
        "type='method_call',interface='org.freedesktop.Notifications',member='Notify'",
        "type='method_return',sender='org.freedesktop.Notifications'",
        "type='signal',sender='org.freedesktop.Notifications',interface='org.freedesktop.Notifications',member='NotificationClosed'",
    };
    QString error = becomeMonitor(m_connection, matches, std::size(matches));

    if (!error.isEmpty()) {
        qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS).noquote() << "Failed to become a DBus monitor."
                                                                 << "No notifications will be sent. Error:" << error;
    }

    // wake up every minute to see if we are still connected
    while (m_connection != nullptr) {
        dbus_connection_read_write_dispatch(m_connection, 60 * 1000);
    }

    deleteLater();
}

void DBusNotificationsListenerThread::stop()
{
    if (m_connection) {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
    }
}

void DBusNotificationsListenerThread::handleMessage(DBusMessage *message)
{
    switch (dbus_message_get_type(message)) {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
        if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "Notify")) {
            handleNotifyCall(message);
        }
        break;
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        handleNotifyReturn(message);
        break;
    case DBUS_MESSAGE_TYPE_SIGNAL:
        if (dbus_message_is_signal(message, "org.freedesktop.Notifications", "NotificationClosed") && dbus_message_has_signature(message, "uu")) {
            DBusMessageIter iter;
            dbus_message_iter_init(message, &iter);
            const uint id = nextUnsigned(&iter);
            const uint reason = nextUnsigned(&iter);
            Q_EMIT notificationClosed(id, reason);
        }
        break;
    default:
        break;
    }
}

void DBusNotificationsListenerThread::handleNotifyCall(DBusMessage *message)
{
    DBusMessageIter iter;
    dbus_message_iter_init(message, &iter);

    if (!dbus_message_has_signature(message, NOTIFY_SIGNATURE)) {
        qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS).nospace()
            << "Call to Notify has wrong signature. Expected " << NOTIFY_SIGNATURE << ", got " << dbus_message_get_signature(message);
        return;
    }

    PendingNotify call;
    call.appName = nextString(&iter);
    call.replacesId = nextUnsigned(&iter);
    call.appIcon = nextString(&iter);
    call.summary = nextString(&iter);
    call.body = nextString(&iter);
    call.actions = nextStringList(&iter);
    call.hints = nextVariantMap(&iter);
    call.timeout = nextInt(&iter);

    const char *sender = dbus_message_get_sender(message);
    if (dbus_message_get_no_reply(message) || !sender) {
        // We will never learn the id the server assigns, only updates of existing notifications can be forwarded
        if (call.replacesId == 0) {
            qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Ignoring Notify call without reply from" << call.appName;
            return;
        }
        emitNotification(call, call.replacesId);
        return;
    }

    // Bound the memory used by callers whose reply we never see
    if (m_pendingNotify.size() > 64) {
        m_pendingNotify.clear();
    }
    m_pendingNotify.insert({QString::fromUtf8(sender), dbus_message_get_serial(message)}, call);
}

void DBusNotificationsListenerThread::handleNotifyReturn(DBusMessage *message)
{
    const char *destination = dbus_message_get_destination(message);
    if (!destination) {
        return;
    }

    const auto key = QPair<QString, dbus_uint32_t>(QString::fromUtf8(destination), dbus_message_get_reply_serial(message));
    auto it = m_pendingNotify.find(key);
    if (it == m_pendingNotify.end()) {
        return;
    }
    const PendingNotify call = it.value();
    m_pendingNotify.erase(it);

    if (!dbus_message_has_signature(message, "u")) {
        return;
    }
    DBusMessageIter iter;
    dbus_message_iter_init(message, &iter);
    const uint id = nextUnsigned(&iter);
    if (id == 0) {
        return;
    }

    emitNotification(call, id);
}

void DBusNotificationsListenerThread::emitNotification(const PendingNotify &call, uint id)
{
    NotificationManager::Notification notification(id);
    notification.setApplicationName(call.appName);
    notification.setApplicationIconName(call.appIcon);
    notification.setSummary(call.summary);
    notification.setBody(call.body); // sanitizes
    notification.setActions(call.actions);
    notification.setTimeout(call.timeout);
    notification.processHints(call.hints);

    Q_EMIT notificationReceived(notification);
}

DBusNotificationsListener::DBusNotificationsListener(KdeConnectPlugin *aPlugin)
    : NotificationsListener(aPlugin)
    , m_thread(new DBusNotificationsListenerThread)
{
    connect(m_thread, &DBusNotificationsListenerThread::notificationReceived, this, &DBusNotificationsListener::onNotify);
    connect(m_thread, &DBusNotificationsListenerThread::notificationClosed, this, &DBusNotificationsListener::onNotificationClosed);
    m_thread->start();
    checkServerCapabilities();
}

DBusNotificationsListener::~DBusNotificationsListener()
{
    m_thread->stop();
    m_thread->quit();
}

void DBusNotificationsListener::checkServerCapabilities()
{
    // org.kde.NotificationManager is only implemented by the Plasma notification server
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.freedesktop.DBus.Introspectable"),
                                                      QStringLiteral("Introspect"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        QDBusPendingReply<QString> reply = *watcher;
        m_canInvokeActions = !reply.isError() && reply.value().contains(QLatin1String("org.kde.NotificationManager"));
        qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Notification server can invoke actions:" << m_canInvokeActions;
    });
}

QByteArray DBusNotificationsListener::iconForNotification(const NotificationManager::Notification &notification) const
{
    QImage image = notification.image();
    if (image.isNull()) {
        // Themed icon name from image-path / app_icon
        QString iconName = notification.icon();
        if (iconName.isEmpty()) {
            iconName = notification.applicationIconName();
        }
        if (!iconName.isEmpty() && !iconName.contains(QLatin1Char('/'))) {
            const QString path = KIconLoader::global()->iconPath(iconName, -KIconLoader::SizeEnormous, true);
            if (!path.isEmpty()) {
                image = QIcon(path).pixmap(KIconLoader::SizeEnormous).toImage();
            }
        }
    }
    if (image.isNull()) {
        return QByteArray();
    }
    if (image.width() > 256 || image.height() > 256) {
        image = image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return pngFromQImage(image);
}

void DBusNotificationsListener::onNotify(const NotificationManager::Notification &notification)
{
    qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Got notification from " << notification.applicationName() << " with id " << notification.id();

    // Drop notifications that the notifications plugin created from a remote device, to avoid echoing them back
    if (notification.hints().contains(QStringLiteral("x-kdeconnect-source-device"))) {
        return;
    }

    auto *config = m_plugin->config();
    if (notification.timeout() > 0 && config->getBool(QStringLiteral("generalPersistent"), false)) {
        return;
    }

    if (!checkApplicationName(notification.applicationName(), notification.applicationIconName())) {
        return;
    }

    int urgency = -1;
    auto urgencyHint = notification.hints().constFind(QStringLiteral("urgency"));
    if (urgencyHint != notification.hints().cend()) {
        bool ok = false;
        urgency = urgencyHint->toInt(&ok);
        if (!ok) {
            urgency = -1;
        }
    }
    if (urgency > -1 && urgency < config->getInt(QStringLiteral("generalUrgency"), 0)) {
        return;
    }

    if (notification.summary().isEmpty()) {
        return;
    }

    const bool includeBody = config->getBool(QStringLiteral("generalIncludeBody"), true);

    ForwardedNotification forwarded;
    forwarded.id = notification.id();
    forwarded.appName = notification.applicationName();
    forwarded.title = notification.summary();
    if (includeBody) {
        forwarded.body = notification.rawBody();
    }

    if (checkIsInBlacklist(forwarded.appName, forwarded.ticker())) {
        return;
    }

    // Without a way to invoke them, buttons on the peer would do nothing
    if (m_canInvokeActions) {
        forwarded.actionIds = notification.actionNames();
        forwarded.actionLabels = notification.actionLabels();
        forwarded.hasDefaultAction = notification.hasDefaultAction();
    }
    forwarded.resident = notification.resident();
    forwarded.urgency = urgency;
    forwarded.category = notification.category();
    forwarded.clearable = true;
    forwarded.time = QDateTime::currentMSecsSinceEpoch();

    auto valueHint = notification.hints().constFind(QStringLiteral("value"));
    if (valueHint != notification.hints().cend()) {
        bool ok = false;
        const int value = valueHint->toInt(&ok);
        if (ok) {
            forwarded.progress = std::clamp(value, 0, 100);
        }
    }

    if (config->getBool(QStringLiteral("generalSynchronizeIcons"), true)) {
        forwarded.icon = iconForNotification(notification);
    }

    qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Sending notification from" << forwarded.appName << " with id " << forwarded.id
                                                 << " summary: " << forwarded.title << " actions: " << forwarded.actionIds;

    const auto previous = m_forwarded.insert(forwarded);
    const bool iconChanged = !previous || previous->iconHash() != forwarded.iconHash();
    // Updates (e.g. progress) should not alert again
    NetworkPacket np = ForwardedNotifications::toPacket(forwarded, previous.has_value(), iconChanged);
    m_plugin->sendPacket(np);
}

void DBusNotificationsListener::onNotificationClosed(uint id, uint reason)
{
    // Expired: the popup timed out but the notification stays in the history. The app stops
    // listening for actions though, so only the buttons go away on the peer.
    if (reason == 1) {
        const ForwardedNotification *forwarded = m_forwarded.find(id);
        if (!forwarded || (forwarded->actionIds.isEmpty() && !forwarded->hasDefaultAction)) {
            return;
        }
        ForwardedNotification expired = *forwarded;
        expired.actionIds.clear();
        expired.actionLabels.clear();
        expired.hasDefaultAction = false;
        m_forwarded.insert(expired);
        NetworkPacket np = ForwardedNotifications::toPacket(expired, true, false);
        m_plugin->sendPacket(np);
        return;
    }

    if (!m_forwarded.remove(id)) {
        return;
    }
    NetworkPacket np = ForwardedNotifications::cancelPacket(id);
    m_plugin->sendPacket(np);
}

void DBusNotificationsListener::invokeAction(uint id, const QString &actionKey)
{
    if (!m_canInvokeActions) {
        qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "The notification server does not allow invoking notification actions, ignoring" << actionKey;
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.kde.NotificationManager"),
                                                      QStringLiteral("InvokeAction"));
    msg.setArguments({id, actionKey});
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void DBusNotificationsListener::closeNotification(uint id)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("CloseNotification"));
    msg.setArguments({id});
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void DBusNotificationsListener::handlePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_NOTIFICATION_ACTION) {
        const auto id = ForwardedNotifications::parseKey(np.get<QString>(QStringLiteral("key")));
        if (!id) {
            return;
        }
        const QString actionKey =
            m_forwarded.resolveAction(*id, np.get<QString>(QStringLiteral("actionId")), np.get<QString>(QStringLiteral("action")));
        if (actionKey.isEmpty()) {
            qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Unknown action for notification" << *id;
            return;
        }
        const bool resident = m_forwarded.find(*id)->resident;
        invokeAction(*id, actionKey);
        // Like the local popup: non-resident notifications go away once an action was invoked
        if (!resident && m_canInvokeActions) {
            closeNotification(*id);
        }
    } else if (np.type() == PACKET_TYPE_NOTIFICATION_REQUEST) {
        if (np.has(QStringLiteral("cancel"))) {
            const auto id = ForwardedNotifications::parseKey(np.get<QString>(QStringLiteral("cancel")));
            if (id && m_forwarded.remove(*id)) {
                closeNotification(*id);
            }
        } else if (np.get<bool>(QStringLiteral("request"))) {
            // The server keeps no list we can query without being its owner, resend what we forwarded
            for (uint id : m_forwarded.ids()) {
                NetworkPacket packet = ForwardedNotifications::toPacket(*m_forwarded.find(id), true, true);
                m_plugin->sendPacket(packet);
            }
        }
    }
}

#include "moc_dbusnotificationslistener.cpp"
