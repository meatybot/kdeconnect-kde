/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "audiostreamsession.h"

#include <QElapsedTimer>
#include <QTimer>

#include <cstring>

namespace AudioStream
{

quint64 monotonicUs()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return quint64(timer.nsecsElapsed() / 1000);
}

OutgoingStream::OutgoingStream(const QString &streamId, const QString &source, AudioCapture *capture, QObject *parent)
    : QObject(parent)
    , m_streamId(streamId)
    , m_source(source)
    , m_capture(capture)
{
    m_capture->setParent(this);
    const StreamFormat f = m_capture->format();
    const qint64 bytesPerMs = qint64(f.sampleRate) * f.channels * f.bytesPerSample() / 1000;
    m_queue.reset(new FrameQueue(bytesPerMs * SenderQueueMs + FrameCodec::HeaderSize * (SenderQueueMs / f.frameDurationMs + 1)));

    connect(m_capture, &AudioCapture::frameCaptured, this, [this](quint64 timestampUs, const QByteArray &pcm) {
        m_queue->enqueue(timestampUs, pcm);
    });
    connect(m_capture, &AudioCapture::failed, this, [this](const QString &error) {
        end(error);
    });
    connect(m_queue.data(), &FrameQueue::opened, this, &OutgoingStream::connected);
    // Closed by the upload job when the receiver disconnects, or by finish() once
    // drained
    connect(m_queue.data(), &QIODevice::aboutToClose, this, [this] {
        // Already closing, end() must not close it again
        m_queue->disconnect(this);
        end({});
    });
}

OutgoingStream::~OutgoingStream()
{
    m_capture->stop();
    m_queue->disconnect(this);
    if (m_queue->isOpen()) {
        m_queue->close();
    }
}

void OutgoingStream::start()
{
    m_capture->start();
}

void OutgoingStream::finish()
{
    if (m_ended) {
        return;
    }
    m_capture->stop();
    if (m_queue->isOpen()) {
        m_queue->finish();
        // Don't wait forever for a receiver that stopped reading
        QTimer::singleShot(FinishTimeoutMs, this, [this] {
            end({});
        });
    } else {
        end({});
    }
}

void OutgoingStream::end(const QString &error)
{
    if (m_ended) {
        return;
    }
    m_ended = true;
    m_capture->stop();
    if (m_queue->isOpen()) {
        m_queue->disconnect(this);
        m_queue->close();
    }
    Q_EMIT finished(error);
}

PlaybackDevice::PlaybackDevice(const StreamFormat &format, int latencyMs, QObject *parent)
    : QIODevice(parent)
    , m_format(format)
    , m_jitter(JitterBuffer::targetFramesFor(latencyMs, format.frameDurationMs), JitterBuffer::maxFramesFor(latencyMs, format.frameDurationMs))
{
}

qint64 PlaybackDevice::bytesAvailable() const
{
    // We always produce data (silence when there is nothing to play)
    return m_format.frameBytes() + QIODevice::bytesAvailable();
}

void PlaybackDevice::push(AudioFrame frame)
{
    QMutexLocker locker(&m_mutex);
    m_jitter.push(std::move(frame));
}

JitterBuffer::Stats PlaybackDevice::stats() const
{
    QMutexLocker locker(&m_mutex);
    return m_jitter.stats();
}

int PlaybackDevice::depth() const
{
    QMutexLocker locker(&m_mutex);
    return m_jitter.depth();
}

JitterBuffer::State PlaybackDevice::state() const
{
    QMutexLocker locker(&m_mutex);
    return m_jitter.state();
}

quint64 PlaybackDevice::silenceBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_silenceBytes;
}

qint64 PlaybackDevice::readData(char *data, qint64 maxSize)
{
    QMutexLocker locker(&m_mutex);
    // Keep sample alignment so a short read never shifts the channels
    const int sampleBytes = m_format.channels * m_format.bytesPerSample();
    maxSize -= maxSize % sampleBytes;

    qint64 written = 0;
    while (written < maxSize) {
        if (m_currentOffset >= m_current.size()) {
            JitterBuffer::Output out = m_jitter.pop();
            m_currentOffset = 0;
            if (out.kind == JitterBuffer::Output::Frame && !out.frame.data.isEmpty()) {
                m_current = std::move(out.frame.data);
                m_current.truncate(m_current.size() - m_current.size() % sampleBytes);
            } else if (out.kind == JitterBuffer::Output::Silence) {
                // Buffering: fill the rest of this read with silence and wait for more
                // frames
                const qint64 rest = maxSize - written;
                memset(data + written, 0, rest);
                m_silenceBytes += rest;
                written += rest;
                m_current.clear();
                break;
            } else {
                m_current = QByteArray(m_format.frameBytes(), '\0');
                m_silenceBytes += m_current.size();
            }
            if (m_current.isEmpty()) {
                continue;
            }
        }
        const qint64 chunk = qMin(maxSize - written, m_current.size() - m_currentOffset);
        memcpy(data + written, m_current.constData() + m_currentOffset, chunk);
        written += chunk;
        m_currentOffset += chunk;
    }
    return written;
}

qint64 PlaybackDevice::writeData(const char *, qint64)
{
    return -1;
}

IncomingStream::IncomingStream(const QString &streamId,
                               const QString &source,
                               const StreamFormat &format,
                               const QSharedPointer<QIODevice> &payload,
                               int latencyMs,
                               QObject *parent)
    : QObject(parent)
    , m_streamId(streamId)
    , m_source(source)
    , m_format(format)
    , m_payload(payload)
    , m_playback(new PlaybackDevice(format, latencyMs, this))
{
    m_playback->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

IncomingStream::~IncomingStream()
{
    if (m_payload) {
        m_payload->disconnect(this);
        m_payload->close();
    }
}

void IncomingStream::start()
{
    if (!m_payload) {
        end(Error::PlaybackFailed);
        return;
    }
    connect(m_payload.data(), &QIODevice::readyRead, this, &IncomingStream::readPayload);
    connect(m_payload.data(), &QIODevice::readChannelFinished, this, [this] {
        readPayload();
        end({});
    });
    connect(m_payload.data(), &QIODevice::aboutToClose, this, [this] {
        end({});
    });
    // Sockets come open; a local device (loopback link) has to be opened by its
    // reader
    if (!m_payload->isOpen() && !m_payload->open(QIODevice::ReadOnly)) {
        end(Error::PlaybackFailed);
        return;
    }
    // The socket may already have data
    readPayload();
}

void IncomingStream::close()
{
    end({});
}

void IncomingStream::readPayload()
{
    if (m_ended || !m_payload) {
        return;
    }
    const QByteArray bytes = m_payload->readAll();
    if (!bytes.isEmpty()) {
        m_decoder.feed(bytes);
    }
    const qint64 now = qint64(monotonicUs());
    while (auto frame = m_decoder.next()) {
        if (frame->last) {
            end({});
            return;
        }
        // Frames that cannot be split into whole samples are corrupt for this
        // format
        if (frame->data.size() % (m_format.channels * m_format.bytesPerSample()) != 0) {
            ++m_framesInvalid;
            continue;
        }
        ++m_framesDecoded;
        // The clock offset between the devices is unknown: track delay relative to
        // the best seen
        const qint64 delay = now - qint64(frame->timestampUs);
        m_minDelayUs = qMin(m_minDelayUs, delay);
        m_jitterUs = delay - m_minDelayUs;
        Q_EMIT frameDecoded(*frame);
        m_playback->push(std::move(*frame));
    }
}

void IncomingStream::end(const QString &error)
{
    if (m_ended) {
        return;
    }
    m_ended = true;
    if (m_payload) {
        m_payload->disconnect(this);
        m_payload->close();
    }
    Q_EMIT finished(error);
}

} // namespace AudioStream

#include "moc_audiocapture.cpp"
#include "moc_audiostreamsession.cpp"
