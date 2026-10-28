/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "audiocapture.h"
#include "audioframe.h"
#include "audiostreamprotocol.h"
#include "framequeue.h"
#include "jitterbuffer.h"

#include <QIODevice>
#include <QMutex>
#include <QPointer>
#include <QSharedPointer>

namespace AudioStream
{

constexpr int SenderQueueMs = 250;
constexpr int DefaultLatencyMs = 80;
constexpr int FinishTimeoutMs = 2000;

// Capture -> FrameQueue (the endless payload of the start packet)
class OutgoingStream : public QObject
{
    Q_OBJECT
public:
    // Takes ownership of capture
    OutgoingStream(const QString &streamId, const QString &source, AudioCapture *capture, QObject *parent = nullptr);
    ~OutgoingStream() override;

    QString streamId() const
    {
        return m_streamId;
    }
    QString source() const
    {
        return m_source;
    }
    StreamFormat format() const
    {
        return m_capture->format();
    }
    QSharedPointer<FrameQueue> payload() const
    {
        return m_queue;
    }

    void start();
    // Send the last frame and let the payload close once it was written
    void finish();
    bool isEnded() const
    {
        return m_ended;
    }

Q_SIGNALS:
    // The receiver connected to the payload
    void connected();
    // The payload was closed (by us or because the receiver went away) or capture
    // failed
    void finished(const QString &error);

private:
    void end(const QString &error);

    const QString m_streamId;
    const QString m_source;
    AudioCapture *const m_capture;
    QSharedPointer<FrameQueue> m_queue;
    bool m_ended = false;
};

// Pull-mode device for QAudioSink: always returns as much PCM as asked, filling
// the gaps the jitter buffer reports (buffering, lost frames) with silence
class PlaybackDevice : public QIODevice
{
    Q_OBJECT
public:
    PlaybackDevice(const StreamFormat &format, int latencyMs, QObject *parent = nullptr);

    bool isSequential() const override
    {
        return true;
    }
    qint64 bytesAvailable() const override;

    // The audio backend may pull from its own thread, so everything below is
    // locked
    void push(AudioFrame frame);
    JitterBuffer::Stats stats() const;
    int depth() const;
    JitterBuffer::State state() const;
    quint64 silenceBytes() const;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    const StreamFormat m_format;
    mutable QMutex m_mutex;
    JitterBuffer m_jitter;
    QByteArray m_current; // frame being played
    qint64 m_currentOffset = 0;
    quint64 m_silenceBytes = 0;
};

// Endless payload -> FrameDecoder -> PlaybackDevice
class IncomingStream : public QObject
{
    Q_OBJECT
public:
    IncomingStream(const QString &streamId,
                   const QString &source,
                   const StreamFormat &format,
                   const QSharedPointer<QIODevice> &payload,
                   int latencyMs = DefaultLatencyMs,
                   QObject *parent = nullptr);
    ~IncomingStream() override;

    QString streamId() const
    {
        return m_streamId;
    }
    QString source() const
    {
        return m_source;
    }
    StreamFormat format() const
    {
        return m_format;
    }
    PlaybackDevice *playbackDevice() const
    {
        return m_playback;
    }
    quint64 framesDecoded() const
    {
        return m_framesDecoded;
    }
    quint64 framesInvalid() const
    {
        return m_framesInvalid;
    }
    double jitterMs() const
    {
        return m_jitterUs / 1000.0;
    }
    int bufferedMs() const
    {
        return m_playback->depth() * m_format.frameDurationMs;
    }

    void start();
    // Close the payload (the sender notices it on its side)
    void close();

Q_SIGNALS:
    void frameDecoded(const AudioStream::AudioFrame &frame);
    // The sender ended the stream (last frame) or the payload closed
    void finished(const QString &error);

private:
    void readPayload();
    void end(const QString &error);

    const QString m_streamId;
    const QString m_source;
    const StreamFormat m_format;
    QSharedPointer<QIODevice> m_payload;
    FrameDecoder m_decoder;
    PlaybackDevice *const m_playback;
    quint64 m_framesDecoded = 0;
    quint64 m_framesInvalid = 0;
    qint64 m_minDelayUs = std::numeric_limits<qint64>::max();
    qint64 m_jitterUs = 0;
    bool m_ended = false;
};

} // namespace AudioStream
