/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 * SPDX-FileCopyrightText: 2018 Simon Redman <simon@ergotech.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "telephonyplugin.h"

#include <KLocalizedString>
#include <QDBusReply>
#include <QDebug>
#include <QUuid>

#include "plugin_telephony_debug.h"
#include <KNotification>
#include <KPluginFactory>
#include <core/daemon.h>

K_PLUGIN_CLASS_WITH_JSON(TelephonyPlugin, "kdeconnect_telephony.json")

void TelephonyPlugin::createNotification(const NetworkPacket &np)
{
    const QString event = np.get<QString>(QStringLiteral("event"));
    const QString phoneNumber = np.get<QString>(QStringLiteral("phoneNumber"), i18n("unknown number"));
    const QString contactName = np.get<QString>(QStringLiteral("contactName"), phoneNumber);
    const QByteArray phoneThumbnail = QByteArray::fromBase64(np.get<QByteArray>(QStringLiteral("phoneThumbnail"), ""));

    QString content, type, icon;

    if (event == QLatin1String("ringing")) {
        type = QStringLiteral("callReceived");
        icon = QStringLiteral("call-start");
        content = i18n("Incoming call from %1", contactName);
    } else if (event == QLatin1String("missedCall")) {
        type = QStringLiteral("missedCall");
        icon = QStringLiteral("call-start");
        content = i18n("Missed call from %1", contactName);
    } else if (event == QLatin1String("talking")) {
        closeCallNotification();
        m_callState = event;
        m_currentPhoneNumber = np.get<QString>(QStringLiteral("phoneNumber"));
        if (!canAnswerCall()) {
            return;
        }
        type = QStringLiteral("callReceived");
        icon = QStringLiteral("call-start");
        content = i18n("Call in progress with %1", contactName);
    } else {
        return;
    }

    if (event != QLatin1String("missedCall")) {
        m_currentPhoneNumber = np.get<QString>(QStringLiteral("phoneNumber"));
        m_callState = event;
    } else {
        m_callState.clear();
    }

    if (event != QLatin1String("talking")) {
        Q_EMIT callReceived(type, phoneNumber, contactName);
    }

    qCDebug(KDECONNECT_PLUGIN_TELEPHONY) << "Creating notification with type:" << type;

    if (!m_currentCallNotification) {
        m_currentCallNotification = new KNotification(type, KNotification::Persistent, this);
    }

    if (!phoneThumbnail.isEmpty()) {
        QPixmap photo;
        photo.loadFromData(phoneThumbnail, "JPEG");
        m_currentCallNotification->setPixmap(photo);
    } else {
        m_currentCallNotification->setIconName(icon);
    }
    m_currentCallNotification->setComponentName(QStringLiteral("kdeconnect"));
    m_currentCallNotification->setTitle(device()->name());
    m_currentCallNotification->setText(content);
    m_currentCallNotification->clearActions();

    const QStringList actions = availableCallActions();
    for (const QString &id : actions) {
        QString label;
        if (id == QLatin1String("answer")) {
            label = i18n("Answer");
        } else if (id == QLatin1String("answer_speaker")) {
            label = i18n("Answer on Speaker");
        } else if (id == QLatin1String("answer_remote")) {
            // Most phones mute other apps' recording during calls, see docs/specs/callhandoff.md
            label = i18n("Answer on Speaker, Try Audio Here");
        } else if (id == QLatin1String("reject")) {
            label = i18n("Reject");
        } else if (id == QLatin1String("mute")) {
            label = i18n("Mute Call");
        } else {
            label = i18n("Hang Up");
        }
        KNotificationAction *action = m_currentCallNotification->addAction(label);
        connect(action, &KNotificationAction::activated, this, [this, id]() {
            triggerCallAction(id);
        });
    }

    m_currentCallNotification->sendEvent();
}

void TelephonyPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_TELEPHONY_ACTION_STATUS) {
        handleActionStatus(np);
        return;
    }

    if (np.get<bool>(QStringLiteral("isCancel"))) {
        closeCallNotification();
        stopCallAudio();
        m_currentPhoneNumber.clear();
        m_callState.clear();
        return;
    }

    // ignore old style sms packet
    if (np.get<QString>(QStringLiteral("event")) != QLatin1String("sms")) {
        createNotification(np);
    }
}

void TelephonyPlugin::handleActionStatus(const NetworkPacket &np)
{
    const QString action = np.get<QString>(QStringLiteral("action"));
    const bool success = np.get<bool>(QStringLiteral("success"));
    const QString error = np.get<QString>(QStringLiteral("error"));

    Q_EMIT callActionStatus(action, success, error);

    if (success) {
        if (action == QLatin1String("answer") && np.get<QString>(QStringLiteral("audioRoute")) == QLatin1String("remote")) {
            startCallAudio();
        }
        return;
    }

    QString reason;
    if (error == QLatin1String("unsupported")) {
        reason = i18n("This is not supported by the Android version of the device");
    } else if (error == QLatin1String("permission_denied")) {
        reason = i18n("KDE Connect on the device is not allowed to manage phone calls");
    } else if (error == QLatin1String("no_call")) {
        reason = i18n("There is no call to act on");
    } else {
        reason = i18n("The device could not perform the action");
    }

    QString text;
    if (action == QLatin1String("answer")) {
        text = i18n("Could not answer the call: %1", reason);
    } else if (action == QLatin1String("reject")) {
        text = i18n("Could not reject the call: %1", reason);
    } else {
        text = i18n("Could not hang up the call: %1", reason);
    }
    Daemon::instance()->sendSimpleNotification(QStringLiteral("error"), device()->name(), text, QStringLiteral("dialog-error"));
}

bool TelephonyPlugin::peerSupports(const QString &packetType) const
{
    return device()->incomingCapabilities().contains(packetType);
}

bool TelephonyPlugin::canAnswerCall() const
{
    return peerSupports(PACKET_TYPE_TELEPHONY_REQUEST_ACTION);
}

bool TelephonyPlugin::canRouteAudioHere() const
{
    return canAnswerCall() && peerSupports(PACKET_TYPE_AUDIOSTREAM_REQUEST);
}

QStringList TelephonyPlugin::availableCallActions() const
{
    QStringList actions;
    if (m_callState == QLatin1String("ringing")) {
        if (canAnswerCall()) {
            actions << QStringLiteral("answer") << QStringLiteral("answer_speaker");
            if (canRouteAudioHere()) {
                actions << QStringLiteral("answer_remote");
            }
            actions << QStringLiteral("reject");
        }
        actions << QStringLiteral("mute");
    } else if (m_callState == QLatin1String("talking") && canAnswerCall()) {
        actions << QStringLiteral("hangup");
    }
    return actions;
}

bool TelephonyPlugin::triggerCallAction(const QString &id)
{
    if (!availableCallActions().contains(id)) {
        return false;
    }
    if (id == QLatin1String("answer")) {
        return answerCall(QStringLiteral("default"));
    } else if (id == QLatin1String("answer_speaker")) {
        return answerCall(QStringLiteral("speaker"));
    } else if (id == QLatin1String("answer_remote")) {
        return answerCall(QStringLiteral("remote"));
    } else if (id == QLatin1String("reject")) {
        return rejectCall();
    } else if (id == QLatin1String("hangup")) {
        return hangUp();
    }
    sendMutePacket();
    return true;
}

bool TelephonyPlugin::answerCall(const QString &audioRoute)
{
    const QString route = audioRoute.isEmpty() ? QStringLiteral("default") : audioRoute;
    if (route != QLatin1String("default") && route != QLatin1String("speaker") && route != QLatin1String("remote")) {
        qCWarning(KDECONNECT_PLUGIN_TELEPHONY) << "Unknown audio route" << audioRoute;
        return false;
    }
    if (route == QLatin1String("remote") && !canRouteAudioHere()) {
        qCWarning(KDECONNECT_PLUGIN_TELEPHONY) << device()->name() << "cannot send call audio to this computer";
        return false;
    }
    return sendAction(QStringLiteral("answer"), route);
}

bool TelephonyPlugin::rejectCall()
{
    return sendAction(QStringLiteral("reject"));
}

bool TelephonyPlugin::hangUp()
{
    return sendAction(QStringLiteral("hangup"));
}

bool TelephonyPlugin::sendAction(const QString &action, const QString &audioRoute)
{
    if (!canAnswerCall()) {
        qCWarning(KDECONNECT_PLUGIN_TELEPHONY) << device()->name() << "does not support call actions";
        return false;
    }

    NetworkPacket np(PACKET_TYPE_TELEPHONY_REQUEST_ACTION, {{QStringLiteral("action"), action}});
    if (!m_currentPhoneNumber.isEmpty()) {
        np.set(QStringLiteral("phoneNumber"), m_currentPhoneNumber);
    }
    if (!audioRoute.isEmpty()) {
        np.set(QStringLiteral("audioRoute"), audioRoute);
    }
    return sendPacket(np);
}

void TelephonyPlugin::startCallAudio()
{
    if (!m_callStreamId.isEmpty() || !peerSupports(PACKET_TYPE_AUDIOSTREAM_REQUEST)) {
        return;
    }
    m_callStreamId = QStringLiteral("call-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    NetworkPacket np(PACKET_TYPE_AUDIOSTREAM_REQUEST,
                     {
                         {QStringLiteral("action"), QStringLiteral("start")},
                         {QStringLiteral("streamId"), m_callStreamId},
                         {QStringLiteral("source"), QStringLiteral("call")},
                         {QStringLiteral("codec"), QStringLiteral("pcm_s16le")},
                         {QStringLiteral("sampleRate"), 48000},
                         {QStringLiteral("channels"), 1},
                     });
    sendPacket(np);
}

void TelephonyPlugin::stopCallAudio()
{
    if (m_callStreamId.isEmpty()) {
        return;
    }
    NetworkPacket np(PACKET_TYPE_AUDIOSTREAM_REQUEST,
                     {
                         {QStringLiteral("action"), QStringLiteral("stop")},
                         {QStringLiteral("streamId"), m_callStreamId},
                     });
    sendPacket(np);
    m_callStreamId.clear();
}

void TelephonyPlugin::closeCallNotification()
{
    if (m_currentCallNotification) {
        m_currentCallNotification->close();
        m_currentCallNotification = nullptr;
    }
}

void TelephonyPlugin::sendMutePacket()
{
    NetworkPacket packet(PACKET_TYPE_TELEPHONY_REQUEST_MUTE, {{QStringLiteral("action"), QStringLiteral("mute")}});
    sendPacket(packet);
}

QString TelephonyPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/telephony").arg(device()->id());
}

#include "moc_telephonyplugin.cpp"
#include "telephonyplugin.moc"
