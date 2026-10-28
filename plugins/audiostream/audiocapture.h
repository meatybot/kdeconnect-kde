/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
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
