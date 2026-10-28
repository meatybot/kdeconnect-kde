/**
 * SPDX-FileCopyrightText: 2019 Richard Liebscher <richard.liebscher@gmail.com>
 * SPDX-FileCopyrightText: 2022 Yoram Bar Haim <bhyoram@protonmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-KDE-Accepted-GPL
 */

#include "mmtelephonyplugin.h"

#include <KLocalizedString>
#include <QDBusPendingCallWatcher>
#include <QDebug>
#include <QLoggingCategory>

#include "callselection.h"
#include "plugin_mmtelephony_debug.h"
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(MMTelephonyPlugin, "kdeconnect_mmtelephony.json")

static const QString PACKET_TYPE_TELEPHONY = QStringLiteral("kdeconnect.telephony");
static const QString PACKET_TYPE_TELEPHONY_REQUEST_MUTE = QStringLiteral("kdeconnect.telephony.request_mute");
static const QString PACKET_TYPE_TELEPHONY_REQUEST_ACTION = QStringLiteral("kdeconnect.telephony.request_action");
static const QString PACKET_TYPE_TELEPHONY_ACTION_STATUS = QStringLiteral("kdeconnect.telephony.action_status");

QSharedPointer<ModemManager::ModemVoice> _voiceInterface(const QSharedPointer<ModemManager::ModemDevice> modemDevice)
{
    return modemDevice->interface(ModemManager::ModemDevice::VoiceInterface).objectCast<ModemManager::ModemVoice>();
}

MMTelephonyPlugin::MMTelephonyPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    connect(ModemManager::notifier(), &ModemManager::Notifier::modemAdded, this, &MMTelephonyPlugin::onModemAdded);
    const auto modems = ModemManager::modemDevices();
    for (const auto &modem : modems) {
        onModemAdded(modem->uni());
    }
}

void MMTelephonyPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_TELEPHONY_REQUEST_ACTION) {
        handleRequestAction(np);
        return;
    }
    if (np.get<QString>(QStringLiteral("event")) == QLatin1String("mute")) {
        // TODO: mute code
    }
}

void MMTelephonyPlugin::onModemAdded(const QString &path)
{
    auto modemDevice = ModemManager::findModemDevice(path);
    if (!modemDevice) {
        return;
    }
    QSharedPointer<ModemManager::ModemVoice> vcm = _voiceInterface(modemDevice);
    if (!vcm || m_watchedVoiceInterfaces.contains(path)) {
        return;
    }
    m_watchedVoiceInterfaces.insert(path);
    auto voice = vcm.get();
    connect(voice, &ModemManager::ModemVoice::callAdded, this, [this, voice](const QString &uni) {
        auto call = voice->findCall(uni);
        onCallAdded(call);
    });
    connect(voice, &ModemManager::ModemVoice::callDeleted, this, [this, voice](const QString &uni) {
        auto call = voice->findCall(uni);
        onCallRemoved(call);
    });
}

void MMTelephonyPlugin::onCallAdded(ModemManager::Call::Ptr call)
{
    qCDebug(KDECONNECT_PLUGIN_MMTELEPHONY) << "Call added" << call->number();

    connect(call.get(), &ModemManager::Call::stateChanged, this, [=, this](MMCallState newState, MMCallState oldState) {
        onCallStateChanged(call.get(), newState, oldState);
    });
}

void MMTelephonyPlugin::onCallRemoved(ModemManager::Call::Ptr call)
{
    if (call) {
        qCDebug(KDECONNECT_PLUGIN_MMTELEPHONY) << "Call removed" << call->number();
    }
}

QString MMTelephonyPlugin::stateName(MMCallState state)
{
    QString event;
    switch (state) {
    case MMCallState::MM_CALL_STATE_RINGING_IN:
        event = QStringLiteral("ringing");
        break;
    case MMCallState::MM_CALL_STATE_ACTIVE:
        event = QStringLiteral("talking");
        break;
    case MMCallState::MM_CALL_STATE_TERMINATED:
        event = QStringLiteral("disconnected");
        break;
    case MMCallState::MM_CALL_STATE_UNKNOWN:
    default:
        event = QStringLiteral("Unknown");
    }
    return event;
}

void MMTelephonyPlugin::onCallStateChanged(ModemManager::Call *call, MMCallState newState, MMCallState oldState)
{
    auto event = stateName(newState);

    qCDebug(KDECONNECT_PLUGIN_MMTELEPHONY) << "Call state changed" << call->uni() << event;
    if (newState != MMCallState::MM_CALL_STATE_TERMINATED) {
        sendMMTelephonyPacket(call, event);
    } else {
        sendCancelMMTelephonyPacket(call, stateName(oldState));
    }
}

void MMTelephonyPlugin::sendMMTelephonyPacket(ModemManager::Call *call, const QString &state)
{
    QString phoneNumber = call->number();

    qCDebug(KDECONNECT_PLUGIN_MMTELEPHONY) << "Phone number is" << phoneNumber;
    NetworkPacket np{PACKET_TYPE_TELEPHONY,
                     {
                         {QStringLiteral("event"), state},
                         {QStringLiteral("phoneNumber"), phoneNumber},
                         {QStringLiteral("contactName"), phoneNumber},
                     }};
    sendPacket(np);
}

void MMTelephonyPlugin::sendCancelMMTelephonyPacket(ModemManager::Call *call, const QString &lastState)
{
    QString phoneNumber = call->number();

    NetworkPacket np{PACKET_TYPE_TELEPHONY,
                     {{QStringLiteral("event"), lastState},
                      {QStringLiteral("phoneNumber"), phoneNumber},
                      {QStringLiteral("contactName"), phoneNumber},
                      {QStringLiteral("isCancel"), true}}};
    sendPacket(np);
}

void MMTelephonyPlugin::handleRequestAction(const NetworkPacket &np)
{
    const QString action = np.get<QString>(QStringLiteral("action"));
    const QString phoneNumber = np.get<QString>(QStringLiteral("phoneNumber"));

    if (action != QLatin1String("answer") && action != QLatin1String("reject") && action != QLatin1String("hangup")) {
        sendActionStatus(action, phoneNumber, QStringLiteral("invalid_action"));
        return;
    }

    QList<ModemManager::Call::Ptr> calls;
    QList<CallSelection::CallInfo> infos;
    const auto modems = ModemManager::modemDevices();
    for (const auto &modem : modems) {
        const auto voice = _voiceInterface(modem);
        if (!voice) {
            continue;
        }
        const auto modemCalls = voice->calls();
        for (const auto &call : modemCalls) {
            calls.append(call);
            infos.append({call->state(), call->direction(), call->number()});
        }
    }

    const int index = CallSelection::selectCall(infos, action, phoneNumber);
    if (index < 0) {
        sendActionStatus(action, phoneNumber, QStringLiteral("no_call"));
        return;
    }

    // ModemManager only controls the call: the speaker is part of the audio stack, and remote
    // audio is requested by the peer from our audiostream plugin, so it needs that plugin loaded.
    QString audioRoute;
    if (action == QLatin1String("answer")) {
        const bool remote = np.get<QString>(QStringLiteral("audioRoute")) == QLatin1String("remote")
            && device()->hasPlugin(QStringLiteral("kdeconnect_audiostream"));
        audioRoute = remote ? QStringLiteral("remote") : QStringLiteral("default");
    }

    const ModemManager::Call::Ptr call = calls.at(index);
    QDBusPendingReply<> reply = action == QLatin1String("answer") ? call->accept() : call->hangup();
    auto watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, action, phoneNumber, audioRoute](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            qCWarning(KDECONNECT_PLUGIN_MMTELEPHONY) << "Call action" << action << "failed:" << w->error().message();
            sendActionStatus(action, phoneNumber, QStringLiteral("failed"));
            return;
        }
        sendActionStatus(action, phoneNumber, QString(), audioRoute);
    });
}

void MMTelephonyPlugin::sendActionStatus(const QString &action, const QString &phoneNumber, const QString &error, const QString &audioRoute)
{
    NetworkPacket np(PACKET_TYPE_TELEPHONY_ACTION_STATUS,
                     {
                         {QStringLiteral("action"), action},
                         {QStringLiteral("success"), error.isEmpty()},
                     });
    if (!error.isEmpty()) {
        np.set(QStringLiteral("error"), error);
    }
    if (!audioRoute.isEmpty()) {
        np.set(QStringLiteral("audioRoute"), audioRoute);
    }
    if (!phoneNumber.isEmpty()) {
        np.set(QStringLiteral("phoneNumber"), phoneNumber);
    }
    sendPacket(np);
}

#include "mmtelephonyplugin.moc"
#include "moc_mmtelephonyplugin.cpp"
