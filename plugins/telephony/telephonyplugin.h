/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 * SPDX-FileCopyrightText: 2018 Simon Redman <simon@ergotech.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QPointer>

#include <KNotification>

#include <core/kdeconnectplugin.h>

/**
 * Packet used for simple telephony events
 *
 * It contains the key "event" which maps to a string indicating the type of event:
 *  - "ringing" - A phone call is incoming
 *  - "talking" - A call is in progress
 *  - "missedCall" - An incoming call was not answered
 *
 * Historically, "sms" was a valid event, but support for that has been dropped in favour
 * of the SMS plugin's more expressive interfaces
 *
 * Depending on the event, other fields may be defined
 */
#define PACKET_TYPE_TELEPHONY QStringLiteral("kdeconnect.telephony")

#define PACKET_TYPE_TELEPHONY_REQUEST_MUTE QStringLiteral("kdeconnect.telephony.request_mute")

/**
 * Asks the phone to act on its current call, see docs/specs/callhandoff.md
 *
 * "action": "answer" | "reject" | "hangup"
 * "phoneNumber": optional, number of the call the user acted on
 * "audioRoute": optional, for "answer" only: "default" | "speaker" | "remote"
 */
#define PACKET_TYPE_TELEPHONY_REQUEST_ACTION QStringLiteral("kdeconnect.telephony.request_action")

/**
 * Reply to PACKET_TYPE_TELEPHONY_REQUEST_ACTION
 *
 * "action": the requested action
 * "success": bool
 * "error": "invalid_action" | "unsupported" | "permission_denied" | "no_call" | "failed"
 * "audioRoute": route applied by a successful "answer"
 */
#define PACKET_TYPE_TELEPHONY_ACTION_STATUS QStringLiteral("kdeconnect.telephony.action_status")

#define PACKET_TYPE_AUDIOSTREAM_REQUEST QStringLiteral("kdeconnect.audiostream.request")

class TelephonyPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.telephony")

public:
    using KdeConnectPlugin::KdeConnectPlugin;

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

    /**
     * Answer the ringing call on the phone. @p audioRoute is "default", "speaker" or "remote"
     * (audio on this computer, needs the audiostream plugin on the phone). Empty means "default".
     * Returns false if the request could not be sent.
     */
    Q_SCRIPTABLE bool answerCall(const QString &audioRoute = QString());
    Q_SCRIPTABLE bool rejectCall();
    Q_SCRIPTABLE bool hangUp();

    /// Whether the phone accepts call actions
    Q_SCRIPTABLE bool canAnswerCall() const;
    /// Whether the phone can send call audio to this computer
    Q_SCRIPTABLE bool canRouteAudioHere() const;

    /**
     * Actions the user can take on the current call, in display order:
     * "answer", "answer_speaker", "answer_remote", "reject", "mute" while ringing, "hangup" while talking
     */
    Q_SCRIPTABLE QStringList availableCallActions() const;
    /// Runs one of availableCallActions(). Returns false if it is not available or could not be sent.
    Q_SCRIPTABLE bool triggerCallAction(const QString &id);

public:
Q_SIGNALS:
    Q_SCRIPTABLE void callReceived(const QString &event, const QString &number, const QString contactName);
    Q_SCRIPTABLE void callActionStatus(const QString &action, bool success, const QString &error);

private Q_SLOTS:
    void sendMutePacket();

private:
    /**
     * Create a notification for:
     *  - Incoming call (with actions to answer, reject or mute the ringing)
     *  - Call in progress (with an action to hang up)
     *  - Missed call
     */
    void createNotification(const NetworkPacket &np);
    void handleActionStatus(const NetworkPacket &np);
    bool sendAction(const QString &action, const QString &audioRoute = QString());
    void startCallAudio();
    void stopCallAudio();
    void closeCallNotification();
    bool peerSupports(const QString &packetType) const;

    QPointer<KNotification> m_currentCallNotification;
    QString m_currentPhoneNumber;
    QString m_callState; // "ringing", "talking" or empty
    QString m_callStreamId;
};
