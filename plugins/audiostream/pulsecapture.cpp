/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pulsecapture.h"

#include "plugin_audiostream_debug.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <QtEndian>

#include <memory>
#include <vector>

namespace AudioStream
{

namespace
{
// Pulse expands this to the monitor of whatever the default sink is when the
// stream connects
const QString DefaultMonitor = QStringLiteral("@DEFAULT_MONITOR@");

struct SimpleDeleter {
    void operator()(pa_simple *s) const
    {
        pa_simple_free(s);
    }
};
using SimplePtr = std::unique_ptr<pa_simple, SimpleDeleter>;
} // namespace

PulseCapture::PulseCapture(const QString &source, int channels, QObject *parent)
    : AudioCapture(parent)
{
    if (source == Source::Microphone) {
        m_devices = {QString()};
    } else if (source == Source::Call) {
        m_devices = {DefaultMonitor, QString()};
    } else {
        m_devices = {DefaultMonitor};
    }
    // Pulse up/downmixes to whatever we ask for
    m_format.channels = qBound(1, channels, 2);
}

int PulseCapture::sourceChannels(const QString &source)
{
    return (source == Source::Media || source == Source::System) ? 2 : 1;
}

PulseCapture::~PulseCapture()
{
    stop();
}

void PulseCapture::start()
{
    if (m_thread) {
        return;
    }
    m_running = true;
    m_thread = QThread::create([this] {
        run();
    });
    m_thread->start();
}

void PulseCapture::stop()
{
    if (!m_thread) {
        return;
    }
    m_running = false;
    // A blocking read returns after at most one frame
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
}

void PulseCapture::run()
{
    const pa_sample_spec spec{PA_SAMPLE_S16LE, uint32_t(m_format.sampleRate), uint8_t(m_format.channels)};
    const uint32_t frameBytes = uint32_t(m_format.frameBytes());
    // Ask for one frame per fragment so the capture latency stays at one frame
    const pa_buffer_attr attr{uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), frameBytes};

    std::vector<SimplePtr> streams;
    for (const QString &device : std::as_const(m_devices)) {
        int error = 0;
        const QByteArray name = device.toUtf8();
        pa_simple *s = pa_simple_new(nullptr,
                                     "KDE Connect",
                                     PA_STREAM_RECORD,
                                     device.isEmpty() ? nullptr : name.constData(),
                                     "Audio stream",
                                     &spec,
                                     nullptr,
                                     &attr,
                                     &error);
        if (!s) {
            qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Could not record from" << (device.isEmpty() ? QStringLiteral("default source") : device) << ":"
                                                     << pa_strerror(error);
            Q_EMIT failed(Error::CaptureFailed);
            return;
        }
        streams.emplace_back(s);
    }

    QByteArray mix(frameBytes, '\0');
    QByteArray other(frameBytes, '\0');
    const qint64 frameUs = qint64(m_format.frameDurationMs) * 1000;
    while (m_running) {
        int error = 0;
        if (pa_simple_read(streams[0].get(), mix.data(), frameBytes, &error) < 0) {
            qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Recording failed:" << pa_strerror(error);
            Q_EMIT failed(Error::CaptureFailed);
            return;
        }
        for (size_t i = 1; i < streams.size(); ++i) {
            if (pa_simple_read(streams[i].get(), other.data(), frameBytes, &error) < 0) {
                qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Recording failed:" << pa_strerror(error);
                Q_EMIT failed(Error::CaptureFailed);
                return;
            }
            uchar *a = reinterpret_cast<uchar *>(mix.data());
            const uchar *b = reinterpret_cast<const uchar *>(other.constData());
            for (uint32_t off = 0; off + 1 < frameBytes; off += 2) {
                const int sum = qFromLittleEndian<qint16>(a + off) + qFromLittleEndian<qint16>(b + off);
                qToLittleEndian<qint16>(qint16(qBound(-32768, sum, 32767)), a + off);
            }
        }
        // The read returns when the last sample of the frame was recorded
        const quint64 now = monotonicUs();
        Q_EMIT frameCaptured(now > quint64(frameUs) ? now - frameUs : 0, mix);
        // Detach: the queued signal holds a reference to the data we just emitted
        mix = QByteArray(frameBytes, '\0');
    }
}

} // namespace AudioStream

#include "moc_pulsecapture.cpp"
