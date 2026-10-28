/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "audiocapture.h"

#include <QThread>

#include <atomic>

namespace AudioStream
{

// Records from PulseAudio (or PipeWire through pipewire-pulse) with
// libpulse-simple on a worker thread. Qt Multimedia does not list sink monitors
// as inputs, hence libpulse. Several devices are mixed sample by sample
// ("call": default sink monitor + default source).
class PulseCapture : public AudioCapture
{
    Q_OBJECT
public:
    // Default sink monitor for media/system, default source for microphone, both
    // for call
    PulseCapture(const QString &source, int channels, QObject *parent = nullptr);

    // Channels a source naturally has: stereo for the sink monitor, mono for
    // microphones
    static int sourceChannels(const QString &source);
    ~PulseCapture() override;

    void start() override;
    void stop() override;
    StreamFormat format() const override
    {
        return m_format;
    }

private:
    void run();

    QStringList m_devices;
    StreamFormat m_format;
    QThread *m_thread = nullptr;
    std::atomic_bool m_running = false;
};

} // namespace AudioStream
