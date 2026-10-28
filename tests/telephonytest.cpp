/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "core/device.h"
#include "core/kdeconnectconfig.h"
#include "core/kdeconnectplugin.h"
#include "testdaemon.h"

#include <QSignalSpy>
#include <QtTest>

#ifdef HAVE_MMTELEPHONY
#include "plugins/mmtelephony/callselection.h"
#endif

// Paired, reachable device that records what plugins send to it
class RecordingDevice : public Device
{
    Q_OBJECT
public:
    using Device::Device;

    bool isReachable() const override
    {
        return true;
    }

    bool sendPacket(NetworkPacket &np) override
    {
        if (!incomingCapabilities().contains(np.type())) {
            return false;
        }
        sent.append(np);
        return true;
    }

    QList<NetworkPacket> sentOfType(const QString &type) const
    {
        QList<NetworkPacket> ret;
        for (const NetworkPacket &np : sent) {
            if (np.type() == type) {
                ret.append(np);
            }
        }
        return ret;
    }

    // Audio requests made for call audio; ignores other plugins' traffic such as the audiostream capability probe
    QList<NetworkPacket> callAudioRequests() const
    {
        QList<NetworkPacket> ret;
        for (const NetworkPacket &np : sentOfType(QStringLiteral("kdeconnect.audiostream.request"))) {
            if (np.get<QString>(QStringLiteral("streamId")).startsWith(QStringLiteral("call-"))) {
                ret.append(np);
            }
        }
        return ret;
    }

    QList<NetworkPacket> sent;
};

static const QString REQUEST_ACTION = QStringLiteral("kdeconnect.telephony.request_action");
static const QString ACTION_STATUS = QStringLiteral("kdeconnect.telephony.action_status");
static const QString AUDIO_REQUEST = QStringLiteral("kdeconnect.audiostream.request");

class TelephonyPluginTest : public QObject
{
    Q_OBJECT

public:
    TelephonyPluginTest()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_daemon = new TestDaemon;
    }

private:
    TestDaemon *m_daemon;
    RecordingDevice *m_device = nullptr;

    // (Re)creates the peer with the given capabilities and returns the loaded plugin
    KdeConnectPlugin *setupPeer(const QSet<QString> &peerIncoming, const QString &pluginName = QStringLiteral("kdeconnect_telephony"))
    {
        delete m_device;
        const QString id = QStringLiteral("callhandoff_test_peer_0123456789abcdef");
        DeviceInfo info(id, KdeConnectConfig::instance().certificate(), QStringLiteral("Test Phone"), DeviceType::Phone);
        KdeConnectConfig::instance().addTrustedDevice(info);

        m_device = new RecordingDevice(this, id);
        info.incomingCapabilities = peerIncoming;
        info.outgoingCapabilities = {QStringLiteral("kdeconnect.telephony"), ACTION_STATUS, REQUEST_ACTION};
        m_device->updateDeviceInfo(info);
        m_device->reloadPlugins();
        return m_device->plugin(pluginName);
    }

    static NetworkPacket telephonyEvent(const QString &event, bool cancel = false)
    {
        NetworkPacket np(QStringLiteral("kdeconnect.telephony"),
                         {{QStringLiteral("event"), event},
                          {QStringLiteral("phoneNumber"), QStringLiteral("+15550100")},
                          {QStringLiteral("contactName"), QStringLiteral("Alice")}});
        if (cancel) {
            np.set(QStringLiteral("isCancel"), true);
        }
        return np;
    }

    static QStringList callActions(KdeConnectPlugin *plugin)
    {
        QStringList ret;
        QMetaObject::invokeMethod(plugin, "availableCallActions", Q_RETURN_ARG(QStringList, ret));
        return ret;
    }

    static bool trigger(KdeConnectPlugin *plugin, const QString &id)
    {
        bool ret = false;
        QMetaObject::invokeMethod(plugin, "triggerCallAction", Q_RETURN_ARG(bool, ret), Q_ARG(QString, id));
        return ret;
    }

    static bool invokeBool(KdeConnectPlugin *plugin, const char *method)
    {
        bool ret = false;
        QMetaObject::invokeMethod(plugin, method, Q_RETURN_ARG(bool, ret));
        return ret;
    }

    static bool answer(KdeConnectPlugin *plugin, const QString &route)
    {
        bool ret = false;
        QMetaObject::invokeMethod(plugin, "answerCall", Q_RETURN_ARG(bool, ret), Q_ARG(QString, route));
        return ret;
    }

private Q_SLOTS:
    void testRequestActionFields()
    {
        KdeConnectPlugin *plugin = setupPeer({REQUEST_ACTION, QStringLiteral("kdeconnect.telephony.request_mute")});
        QVERIFY(plugin);
        QVERIFY(invokeBool(plugin, "canAnswerCall"));
        QVERIFY(!invokeBool(plugin, "canRouteAudioHere"));

        plugin->receivePacket(telephonyEvent(QStringLiteral("ringing")));
        QCOMPARE(callActions(plugin), QStringList({QStringLiteral("answer"), QStringLiteral("answer_speaker"), QStringLiteral("reject"), QStringLiteral("mute")}));
        QVERIFY(!trigger(plugin, QStringLiteral("answer_remote")));
        QVERIFY(!trigger(plugin, QStringLiteral("hangup")));

        QVERIFY(answer(plugin, QString()));
        QVERIFY(trigger(plugin, QStringLiteral("answer_speaker")));
        QVERIFY(trigger(plugin, QStringLiteral("reject")));
        QVERIFY(trigger(plugin, QStringLiteral("mute")));
        QCOMPARE(m_device->sentOfType(QStringLiteral("kdeconnect.telephony.request_mute")).size(), 1);

        const auto sent = m_device->sentOfType(REQUEST_ACTION);
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent[0].get<QString>(QStringLiteral("action")), QStringLiteral("answer"));
        QCOMPARE(sent[0].get<QString>(QStringLiteral("audioRoute")), QStringLiteral("default"));
        QCOMPARE(sent[0].get<QString>(QStringLiteral("phoneNumber")), QStringLiteral("+15550100"));
        QCOMPARE(sent[1].get<QString>(QStringLiteral("audioRoute")), QStringLiteral("speaker"));
        QCOMPARE(sent[2].get<QString>(QStringLiteral("action")), QStringLiteral("reject"));
        QVERIFY(!sent[2].has(QStringLiteral("audioRoute")));

        // Talking replaces the ringing actions with a hang up action
        plugin->receivePacket(telephonyEvent(QStringLiteral("talking")));
        QCOMPARE(callActions(plugin), QStringList({QStringLiteral("hangup")}));
        QVERIFY(trigger(plugin, QStringLiteral("hangup")));
        const NetworkPacket hangup = m_device->sentOfType(REQUEST_ACTION).last();
        QCOMPARE(hangup.get<QString>(QStringLiteral("action")), QStringLiteral("hangup"));
        QCOMPARE(hangup.get<QString>(QStringLiteral("phoneNumber")), QStringLiteral("+15550100"));
    }

    void testInvalidRoutes()
    {
        KdeConnectPlugin *plugin = setupPeer({REQUEST_ACTION});
        QVERIFY(plugin);
        QVERIFY(!answer(plugin, QStringLiteral("bogus")));
        // No audiostream on the peer: the audio cannot come here
        QVERIFY(!answer(plugin, QStringLiteral("remote")));
        QVERIFY(m_device->sentOfType(REQUEST_ACTION).isEmpty());
    }

    void testOldPeerWithoutRequestAction()
    {
        KdeConnectPlugin *plugin = setupPeer({QStringLiteral("kdeconnect.telephony.request_mute")});
        QVERIFY(plugin);
        QVERIFY(!invokeBool(plugin, "canAnswerCall"));

        plugin->receivePacket(telephonyEvent(QStringLiteral("ringing")));
        QCOMPARE(callActions(plugin), QStringList({QStringLiteral("mute")}));
        QVERIFY(!trigger(plugin, QStringLiteral("answer")));

        QVERIFY(!answer(plugin, QString()));
        QVERIFY(!invokeBool(plugin, "rejectCall"));
        QVERIFY(!invokeBool(plugin, "hangUp"));
        QCOMPARE(m_device->sent.size(), 0);
    }

    void testFailureStatus()
    {
        KdeConnectPlugin *plugin = setupPeer({REQUEST_ACTION, AUDIO_REQUEST});
        QVERIFY(plugin);
        QSignalSpy spy(plugin, SIGNAL(callActionStatus(QString, bool, QString)));

        NetworkPacket status(ACTION_STATUS,
                             {{QStringLiteral("action"), QStringLiteral("answer")},
                              {QStringLiteral("success"), false},
                              {QStringLiteral("error"), QStringLiteral("permission_denied")}});
        plugin->receivePacket(status);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy[0][0].toString(), QStringLiteral("answer"));
        QCOMPARE(spy[0][1].toBool(), false);
        QCOMPARE(spy[0][2].toString(), QStringLiteral("permission_denied"));
        // A failed answer must not start call audio
        QVERIFY(m_device->callAudioRequests().isEmpty());
    }

    void testRemoteAudioRequest()
    {
        KdeConnectPlugin *plugin = setupPeer({REQUEST_ACTION, AUDIO_REQUEST});
        QVERIFY(plugin);
        QVERIFY(invokeBool(plugin, "canRouteAudioHere"));

        plugin->receivePacket(telephonyEvent(QStringLiteral("ringing")));
        QVERIFY(callActions(plugin).contains(QStringLiteral("answer_remote")));
        QVERIFY(answer(plugin, QStringLiteral("remote")));
        QCOMPARE(m_device->sentOfType(REQUEST_ACTION).last().get<QString>(QStringLiteral("audioRoute")), QStringLiteral("remote"));
        // Nothing is requested before the phone confirmed
        QVERIFY(m_device->callAudioRequests().isEmpty());

        NetworkPacket status(ACTION_STATUS,
                             {{QStringLiteral("action"), QStringLiteral("answer")},
                              {QStringLiteral("success"), true},
                              {QStringLiteral("audioRoute"), QStringLiteral("remote")}});
        plugin->receivePacket(status);
        // A duplicate status must not start a second stream
        plugin->receivePacket(status);

        auto audio = m_device->callAudioRequests();
        QCOMPARE(audio.size(), 1);
        const NetworkPacket start = audio[0];
        QCOMPARE(start.get<QString>(QStringLiteral("action")), QStringLiteral("start"));
        QCOMPARE(start.get<QString>(QStringLiteral("source")), QStringLiteral("call"));
        QCOMPARE(start.get<QString>(QStringLiteral("codec")), QStringLiteral("pcm_s16le"));
        QCOMPARE(start.get<int>(QStringLiteral("sampleRate")), 48000);
        QCOMPARE(start.get<int>(QStringLiteral("channels")), 1);
        const QString streamId = start.get<QString>(QStringLiteral("streamId"));
        QVERIFY(streamId.startsWith(QStringLiteral("call-")));

        // Call ends on the phone
        plugin->receivePacket(telephonyEvent(QStringLiteral("talking"), true));
        audio = m_device->callAudioRequests();
        QCOMPARE(audio.size(), 2);
        QCOMPARE(audio[1].get<QString>(QStringLiteral("action")), QStringLiteral("stop"));
        QCOMPARE(audio[1].get<QString>(QStringLiteral("streamId")), streamId);

        // A status with "default" route (phone could not route remotely) starts nothing
        status.set(QStringLiteral("audioRoute"), QStringLiteral("default"));
        plugin->receivePacket(status);
        QCOMPARE(m_device->callAudioRequests().size(), 2);
    }

#ifdef HAVE_MMTELEPHONY
    void testCallSelection()
    {
        using CallSelection::CallInfo;
        const QList<CallInfo> calls{
            {MM_CALL_STATE_ACTIVE, MM_CALL_DIRECTION_OUTGOING, QStringLiteral("111")},
            {MM_CALL_STATE_RINGING_IN, MM_CALL_DIRECTION_INCOMING, QStringLiteral("222")},
            {MM_CALL_STATE_RINGING_IN, MM_CALL_DIRECTION_INCOMING, QStringLiteral("333")},
            {MM_CALL_STATE_TERMINATED, MM_CALL_DIRECTION_INCOMING, QStringLiteral("444")},
        };
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("answer"), QString()), 1);
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("answer"), QStringLiteral("333")), 2);
        // Unknown number falls back to the first ringing call
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("reject"), QStringLiteral("999")), 1);
        // The active call cannot be "answered"
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("answer"), QStringLiteral("111")), 1);
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("hangup"), QString()), 0);
        QCOMPARE(CallSelection::selectCall(calls, QStringLiteral("dance"), QString()), -1);

        const QList<CallInfo> onlyRinging{{MM_CALL_STATE_RINGING_IN, MM_CALL_DIRECTION_INCOMING, QStringLiteral("222")}};
        QCOMPARE(CallSelection::selectCall(onlyRinging, QStringLiteral("hangup"), QString()), 0);

        const QList<CallInfo> ended{{MM_CALL_STATE_TERMINATED, MM_CALL_DIRECTION_INCOMING, QStringLiteral("444")}};
        QCOMPARE(CallSelection::selectCall(ended, QStringLiteral("answer"), QString()), -1);
        QCOMPARE(CallSelection::selectCall(ended, QStringLiteral("hangup"), QString()), -1);
    }

    void testModemPluginStatus()
    {
        KdeConnectPlugin *plugin = setupPeer({QStringLiteral("kdeconnect.telephony"), ACTION_STATUS}, QStringLiteral("kdeconnect_mmtelephony"));
        if (!plugin) {
            QSKIP("kdeconnect_mmtelephony is not available");
        }

        plugin->receivePacket(NetworkPacket(REQUEST_ACTION, {{QStringLiteral("action"), QStringLiteral("explode")}}));
        // The test environment has no modem with calls
        plugin->receivePacket(NetworkPacket(REQUEST_ACTION, {{QStringLiteral("action"), QStringLiteral("answer")}, {QStringLiteral("phoneNumber"), QStringLiteral("+15550100")}}));

        const auto statuses = m_device->sentOfType(ACTION_STATUS);
        QCOMPARE(statuses.size(), 2);
        QCOMPARE(statuses[0].get<QString>(QStringLiteral("action")), QStringLiteral("explode"));
        QCOMPARE(statuses[0].get<bool>(QStringLiteral("success")), false);
        QCOMPARE(statuses[0].get<QString>(QStringLiteral("error")), QStringLiteral("invalid_action"));
        QCOMPARE(statuses[1].get<QString>(QStringLiteral("action")), QStringLiteral("answer"));
        QCOMPARE(statuses[1].get<bool>(QStringLiteral("success")), false);
        QCOMPARE(statuses[1].get<QString>(QStringLiteral("error")), QStringLiteral("no_call"));
        QCOMPARE(statuses[1].get<QString>(QStringLiteral("phoneNumber")), QStringLiteral("+15550100"));
    }
#endif
};

QTEST_MAIN(TelephonyPluginTest);

#include "telephonytest.moc"
