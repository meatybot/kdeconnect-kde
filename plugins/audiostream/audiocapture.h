/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "audiostreamprotocol.h"

#include <QByteArray>
#include <QObject>

namespace AudioStream
{

// Monotonic clock used for frame timestamps, in microseconds
quint64 monotonicUs();

// Something that produces PCM frames of format().frameBytes() bytes each
class AudioCapture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual StreamFormat format() const = 0;

Q_SIGNALS:
    void frameCaptured(quint64 timestampUs, const QByteArray &pcm);
    void failed(const QString &error);
};

} // namespace AudioStream
