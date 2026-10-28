/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <ModemManager/ModemManager.h>
#include <QList>
#include <QString>

namespace CallSelection
{
struct CallInfo {
    MMCallState state;
    MMCallDirection direction;
    QString number;
};

inline bool isRinging(const CallInfo &call)
{
    return call.state == MM_CALL_STATE_RINGING_IN && call.direction == MM_CALL_DIRECTION_INCOMING;
}

inline bool isOngoing(const CallInfo &call)
{
    return call.state != MM_CALL_STATE_TERMINATED && call.state != MM_CALL_STATE_UNKNOWN;
}

/**
 * Pick the call a kdeconnect.telephony.request_action applies to.
 * "answer" and "reject" need a ringing incoming call, "hangup" prefers an active call
 * and falls back to any ongoing one. Calls matching @p phoneNumber win.
 * Returns the index in @p calls or -1.
 */
inline int selectCall(const QList<CallInfo> &calls, const QString &action, const QString &phoneNumber)
{
    const auto pick = [&](auto predicate) {
        int fallback = -1;
        for (int i = 0; i < calls.size(); ++i) {
            if (!predicate(calls[i])) {
                continue;
            }
            if (!phoneNumber.isEmpty() && calls[i].number == phoneNumber) {
                return i;
            }
            if (fallback < 0) {
                fallback = i;
            }
        }
        return fallback;
    };

    if (action == QLatin1String("answer") || action == QLatin1String("reject")) {
        return pick(isRinging);
    }
    if (action == QLatin1String("hangup")) {
        const int active = pick([](const CallInfo &call) {
            return call.state == MM_CALL_STATE_ACTIVE;
        });
        return active >= 0 ? active : pick(isOngoing);
    }
    return -1;
}
}
