/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "kdeconnectplugin.h"
#include "testdaemon.h"

#include <plugins/sendnotifications/forwardednotifications.h>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QtTest>

namespace
{
const QString s_serverService = QStringLiteral("org.freedesktop.Notifications");
const QString s_serverPath = QStringLiteral("/org/freedesktop/Notifications");

// Minimal notification server implementing the bits of the Plasma one that the plugins use
class FakeNotificationServer : public QDBusVirtualObject
{
    Q_OBJECT

public:
    explicit FakeNotificationServer(const QDBusConnection &connection)
        : m_connection(connection)
    {
    }

    QString introspect(const QString &path) const override
    {
        Q_UNUSED(path);
        return QStringLiteral(
            "<interface name=\"org.freedesktop.Notifications\">"
            "<method name=\"Notify\"><arg type=\"u\" direction=\"out\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/>"
            "<arg type=\"a{sv}\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
            "<method name=\"CloseNotification\"><arg type=\"u\" direction=\"in\"/></method>"
            "<method name=\"GetCapabilities\"><arg type=\"as\" direction=\"out\"/></method>"
            "</interface>"
            "<interface name=\"org.kde.NotificationManager\">"
            "<method name=\"InvokeAction\"><arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
            "</interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        const QString member = message.member();
        if (message.interface() == QLatin1String("org.freedesktop.DBus.Introspectable")) {
            const QString xml = QStringLiteral("<node>") + introspect(message.path()) + QStringLiteral("</node>");
            connection.send(message.createReply(xml));
        } else if (member == QLatin1String("Notify")) {
            const uint replacesId = message.arguments().at(1).toUInt();
            const uint id = replacesId ? replacesId : ++m_lastId;
            notified.append({id, message.arguments().at(3).toString()});
            connection.send(message.createReply(id));
        } else if (member == QLatin1String("CloseNotification")) {
            const uint id = message.arguments().at(0).toUInt();
            closed.append(id);
            connection.send(message.createReply());
            emitClosed(id, 3);
        } else if (member == QLatin1String("InvokeAction")) {
            invoked.append({message.arguments().at(0).toUInt(), message.arguments().at(1).toString()});
            connection.send(message.createReply());
        } else if (member == QLatin1String("GetCapabilities")) {
            connection.send(message.createReply(QStringList{QStringLiteral("actions"), QStringLiteral("body")}));
        } else {
            return false;
        }
        return true;
    }

    void emitClosed(uint id, uint reason)
    {
        QDBusMessage signal = QDBusMessage::createSignal(s_serverPath, s_serverService, QStringLiteral("NotificationClosed"));
        signal.setArguments({id, reason});
        m_connection.send(signal);
    }

    QList<QPair<uint, QString>> notified; // id, summary
    QList<QPair<uint, QString>> invoked; // id, action
    QList<uint> closed;

private:
    QDBusConnection m_connection;
    uint m_lastId = 0;
};

ForwardedNotification sample()
{
    ForwardedNotification n;
    n.id = 42;
    n.appName = QStringLiteral("Mail");
    n.title = QStringLiteral("New message");
    n.body = QStringLiteral("Hello there");
    n.actionIds = {QStringLiteral("archive"), QStringLiteral("mark-read")};
    n.actionLabels = {QStringLiteral("Archive"), QStringLiteral("Mark as read")};
    n.hasDefaultAction = true;
    n.urgency = 2;
    n.progress = 30;
    n.category = QStringLiteral("email.arrived");
    n.time = 1700000000000;
    n.icon = QByteArray("\x89PNG fake icon bytes", 20);
    return n;
}
}

class RichNotificationsTest : public QObject
{
    Q_OBJECT

public:
    RichNotificationsTest()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

private:
    QObject *findForwarded(KdeConnectPlugin *receiver, const QString &id)
    {
        const auto children = receiver->findChildren<QObject *>();
        for (QObject *child : children) {
            if (qstrcmp(child->metaObject()->className(), "Notification") == 0 && child->property("internalId").toString() == id) {
                return child;
            }
        }
        return nullptr;
    }

    uint notifyAsApp(const QString &summary, const QStringList &actions)
    {
        // Through the bus (not the server's own connection), so the monitor sees call and reply
        QDBusMessage msg = QDBusMessage::createMethodCall(s_serverService, s_serverPath, s_serverService, QStringLiteral("Notify"));
        msg.setArguments({QStringLiteral("Mail"), 0u, QString(), summary, QStringLiteral("Body"), actions, QVariantMap(), -1});
        // The fake server lives in this thread, so keep the event loop running
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::BlockWithGui);
        return reply.arguments().value(0).toUInt();
    }

private Q_SLOTS:
    void testPacketWithActions()
    {
        const ForwardedNotification n = sample();
        NetworkPacket np = ForwardedNotifications::toPacket(n, false, true);

        QCOMPARE(np.type(), PACKET_TYPE_NOTIFICATION);
        QCOMPARE(np.get<QString>(QStringLiteral("id")), QStringLiteral("42"));
        QCOMPARE(np.get<QString>(QStringLiteral("appName")), QStringLiteral("Mail"));
        QCOMPARE(np.get<QString>(QStringLiteral("title")), QStringLiteral("New message"));
        QCOMPARE(np.get<QString>(QStringLiteral("text")), QStringLiteral("Hello there"));
        QCOMPARE(np.get<QString>(QStringLiteral("ticker")), QStringLiteral("New message: Hello there"));
        QCOMPARE(np.get<bool>(QStringLiteral("silent")), false);
        QCOMPARE(np.get<bool>(QStringLiteral("isClearable")), true);
        QCOMPARE(np.get<QString>(QStringLiteral("time")), QStringLiteral("1700000000000"));
        QCOMPARE(np.get<int>(QStringLiteral("urgency")), 2);
        QCOMPARE(np.get<int>(QStringLiteral("progress")), 30);
        QCOMPARE(np.get<QString>(QStringLiteral("category")), QStringLiteral("email.arrived"));
        QCOMPARE(np.get<bool>(QStringLiteral("hasDefaultAction")), true);

        // Legacy "actions" keeps carrying labels, ids go in the parallel array
        QCOMPARE(np.get<QStringList>(QStringLiteral("actions")), n.actionLabels);
        QCOMPARE(np.get<QStringList>(QStringLiteral("actionIds")), n.actionIds);

        QVERIFY(np.hasPayload());
        QCOMPARE(np.payloadSize(), n.icon.size());
        QCOMPARE(np.get<QString>(QStringLiteral("payloadHash")), n.iconHash());
        QVERIFY(np.payload()->open(QIODevice::ReadOnly));
        QCOMPARE(np.payload()->readAll(), n.icon);
    }

    void testPacketSurvivesSerialization()
    {
        NetworkPacket np = ForwardedNotifications::toPacket(sample(), true, false);
        QVERIFY(!np.hasPayload());
        // The hash is still there so the peer can reuse its cached icon
        QVERIFY(!np.get<QString>(QStringLiteral("payloadHash")).isEmpty());

        NetworkPacket parsed;
        QVERIFY(NetworkPacket::unserialize(np.serialize(), &parsed));
        QCOMPARE(parsed.get<QStringList>(QStringLiteral("actions")), sample().actionLabels);
        QCOMPARE(parsed.get<QStringList>(QStringLiteral("actionIds")), sample().actionIds);
        QCOMPARE(parsed.get<bool>(QStringLiteral("silent")), true);
    }

    void testPlainNotificationHasNoRichFields()
    {
        ForwardedNotification n;
        n.id = 7;
        n.appName = QStringLiteral("App");
        n.title = QStringLiteral("Title");
        NetworkPacket np = ForwardedNotifications::toPacket(n, false, true);

        QCOMPARE(np.get<QString>(QStringLiteral("ticker")), QStringLiteral("Title"));
        const QStringList optional = {QStringLiteral("text"),
                                      QStringLiteral("actions"),
                                      QStringLiteral("actionIds"),
                                      QStringLiteral("hasDefaultAction"),
                                      QStringLiteral("resident"),
                                      QStringLiteral("urgency"),
                                      QStringLiteral("progress"),
                                      QStringLiteral("category"),
                                      QStringLiteral("payloadHash")};
        for (const QString &key : optional) {
            QVERIFY2(!np.has(key), qPrintable(key));
        }
        QVERIFY(!np.hasPayload());
    }

    void testCancelPacket()
    {
        NetworkPacket np = ForwardedNotifications::cancelPacket(42);
        QCOMPARE(np.type(), PACKET_TYPE_NOTIFICATION);
        QCOMPARE(np.get<QString>(QStringLiteral("id")), QStringLiteral("42"));
        QCOMPARE(np.get<bool>(QStringLiteral("isCancel")), true);
    }

    void testResolveAction()
    {
        ForwardedNotifications store;
        store.insert(sample());

        // New peers send the id
        QCOMPARE(store.resolveAction(42, QStringLiteral("mark-read"), QStringLiteral("whatever")), QStringLiteral("mark-read"));
        QCOMPARE(store.resolveAction(42, QStringLiteral("default"), QString()), QStringLiteral("default"));
        // Legacy peers only send the label
        QCOMPARE(store.resolveAction(42, QString(), QStringLiteral("Archive")), QStringLiteral("archive"));
        // Anything not offered is rejected
        QCOMPARE(store.resolveAction(42, QStringLiteral("delete"), QString()), QString());
        QCOMPARE(store.resolveAction(42, QString(), QStringLiteral("Delete")), QString());
        QCOMPARE(store.resolveAction(43, QStringLiteral("archive"), QString()), QString());

        ForwardedNotification noDefault = sample();
        noDefault.id = 5;
        noDefault.hasDefaultAction = false;
        store.insert(noDefault);
        QCOMPARE(store.resolveAction(5, QStringLiteral("default"), QString()), QString());
    }

    void testInsertUpdateRemove()
    {
        ForwardedNotifications store;
        QVERIFY(!store.insert(sample()).has_value());

        ForwardedNotification updated = sample();
        updated.progress = 80;
        const auto previous = store.insert(updated);
        QVERIFY(previous.has_value());
        QCOMPARE(previous->progress, 30);
        QCOMPARE(store.find(42)->progress, 80);
        QCOMPARE(store.ids(), QList<uint>{42});

        QVERIFY(store.remove(42));
        QVERIFY(!store.remove(42));
        QVERIFY(!store.find(42));
        QCOMPARE(store.resolveAction(42, QStringLiteral("archive"), QString()), QString());
    }

    void testStoreIsBounded()
    {
        ForwardedNotifications store;
        for (uint id = 1; id <= ForwardedNotifications::s_maxTracked + 10; ++id) {
            ForwardedNotification n;
            n.id = id;
            store.insert(n);
        }
        QCOMPARE(store.ids().size(), ForwardedNotifications::s_maxTracked);
        QVERIFY(!store.find(1));
        QVERIFY(!store.find(10));
        QVERIFY(store.find(11));
        QCOMPARE(store.ids().constLast(), uint(ForwardedNotifications::s_maxTracked + 10));
    }

    // Local notification -> loopback peer -> action/dismissal back to the local server
    void testRoundTripThroughLoopback()
    {
        QDBusConnection serverBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fakeNotificationServer"));
        QVERIFY(serverBus.isConnected());
        FakeNotificationServer server(serverBus);
        QVERIFY(serverBus.registerVirtualObject(s_serverPath, &server));
        QVERIFY(serverBus.registerService(s_serverService));

        // Never deleted, like in the other tests
        auto *daemon = new TestDaemon;
        Device *device = nullptr;
        const auto deviceIds = daemon->devices();
        for (const QString &deviceId : deviceIds) {
            Device *d = daemon->getDevice(deviceId);
            if (d->isReachable()) {
                if (!d->isPaired()) {
                    d->requestPairing();
                }
                device = d;
            }
        }
        QVERIFY(device);
        QVERIFY(device->isPaired());

        device->setPluginEnabled(QStringLiteral("kdeconnect_sendnotifications"), true);
        device->setPluginEnabled(QStringLiteral("kdeconnect_notifications"), true);
        KdeConnectPlugin *sender = device->plugin(QStringLiteral("kdeconnect_sendnotifications"));
        KdeConnectPlugin *receiver = device->plugin(QStringLiteral("kdeconnect_notifications"));
        QVERIFY(sender);
        QVERIFY(receiver);

        // The monitor thread and the capability check start asynchronously, retry until both are up
        const QStringList actions = {QStringLiteral("archive"), QStringLiteral("Archive"), QStringLiteral("default"), QString()};
        uint id = 0;
        QObject *forwarded = nullptr;
        for (int attempt = 0; attempt < 20 && !forwarded; ++attempt) {
            id = notifyAsApp(QStringLiteral("Attempt %1").arg(attempt), actions);
            QVERIFY(id != 0);
            (void)QTest::qWaitFor(
                [&] {
                    return findForwarded(receiver, QString::number(id)) != nullptr;
                },
                500);
            forwarded = findForwarded(receiver, QString::number(id));
            if (forwarded && forwarded->property("actions").toStringList().isEmpty()) {
                // Forwarded before the server was known to support actions
                forwarded = nullptr;
            }
        }
        QVERIFY(forwarded);
        QCOMPARE(forwarded->property("actions").toStringList(), QStringList{QStringLiteral("Archive")});
        QCOMPARE(forwarded->property("dismissable").toBool(), true);

        // New peer: action id
        QVERIFY(QMetaObject::invokeMethod(receiver,
                                          "sendActionWithId",
                                          Q_ARG(QString, QString::number(id)),
                                          Q_ARG(QString, QStringLiteral("Archive")),
                                          Q_ARG(QString, QStringLiteral("archive"))));
        QTRY_VERIFY(server.invoked.contains(qMakePair(id, QStringLiteral("archive"))));
        // Non-resident: closed after the action, and the close is synced back to the peer
        QTRY_VERIFY(server.closed.contains(id));
        QTRY_VERIFY(!findForwarded(receiver, QString::number(id)));

        // Legacy peer: label only
        const uint legacyId = notifyAsApp(QStringLiteral("Legacy"), {QStringLiteral("reply-later"), QStringLiteral("Later")});
        QTRY_VERIFY(findForwarded(receiver, QString::number(legacyId)));
        QVERIFY(QMetaObject::invokeMethod(receiver, "sendAction", Q_ARG(QString, QString::number(legacyId)), Q_ARG(QString, QStringLiteral("Later"))));
        QTRY_VERIFY(server.invoked.contains(qMakePair(legacyId, QStringLiteral("reply-later"))));

        // Unknown actions are not passed to the server
        const uint guardedId = notifyAsApp(QStringLiteral("Guarded"), {QStringLiteral("ok"), QStringLiteral("OK")});
        QTRY_VERIFY(findForwarded(receiver, QString::number(guardedId)));
        const qsizetype invokedBefore = server.invoked.size();
        QVERIFY(QMetaObject::invokeMethod(receiver,
                                          "sendActionWithId",
                                          Q_ARG(QString, QString::number(guardedId)),
                                          Q_ARG(QString, QStringLiteral("Delete")),
                                          Q_ARG(QString, QStringLiteral("delete"))));
        QTest::qWait(200);
        QCOMPARE(server.invoked.size(), invokedBefore);

        // Dismissed on the peer -> closed on the server
        QVERIFY(QMetaObject::invokeMethod(findForwarded(receiver, QString::number(guardedId)), "dismiss"));
        QTRY_VERIFY(server.closed.contains(guardedId));

        // Closed locally (e.g. by the app) -> removed on the peer
        const uint closedId = notifyAsApp(QStringLiteral("Closed locally"), {});
        QTRY_VERIFY(findForwarded(receiver, QString::number(closedId)));
        server.emitClosed(closedId, 2);
        QTRY_VERIFY(!findForwarded(receiver, QString::number(closedId)));

        // Expired popups stay in the history: kept on the peer, but without actions the app no longer handles
        const uint expiredId = notifyAsApp(QStringLiteral("Expiring"), {QStringLiteral("ok"), QStringLiteral("OK")});
        QTRY_VERIFY(findForwarded(receiver, QString::number(expiredId)));
        QCOMPARE(findForwarded(receiver, QString::number(expiredId))->property("actions").toStringList(), QStringList{QStringLiteral("OK")});
        server.emitClosed(expiredId, 1);
        QTRY_VERIFY(findForwarded(receiver, QString::number(expiredId))->property("actions").toStringList().isEmpty());
        const qsizetype invokedBeforeExpired = server.invoked.size();
        QVERIFY(QMetaObject::invokeMethod(receiver, "sendAction", Q_ARG(QString, QString::number(expiredId)), Q_ARG(QString, QStringLiteral("OK"))));
        QTest::qWait(200);
        QCOMPARE(server.invoked.size(), invokedBeforeExpired);

        device->setPluginEnabled(QStringLiteral("kdeconnect_sendnotifications"), false);
        serverBus.unregisterService(s_serverService);
        serverBus.unregisterObject(s_serverPath);
    }

    void testParseKey()
    {
        QCOMPARE(ForwardedNotifications::parseKey(QStringLiteral("42")), std::optional<uint>(42));
        QCOMPARE(ForwardedNotifications::parseKey(QStringLiteral("0")), std::nullopt);
        QCOMPARE(ForwardedNotifications::parseKey(QStringLiteral("-1")), std::nullopt);
        // Android keys are never ours
        QCOMPARE(ForwardedNotifications::parseKey(QStringLiteral("0|com.example|1|null|10123")), std::nullopt);
    }
};

QTEST_MAIN(RichNotificationsTest);

#include "richnotificationstest.moc"
