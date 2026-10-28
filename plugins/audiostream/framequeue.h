/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "audioframe.h"

#include <QIODevice>

#include <deque>

namespace AudioStream
{

// Sequential device used as the endless payload of an outgoing stream. Captured
// frames are queued encoded; if the network does not drain the queue, the
// oldest frames that have not started being read are dropped so latency stays
// bounded.
class FrameQueue : public QIODevice
{
    Q_OBJECT
public:
    explicit FrameQueue(qint64 maxQueuedBytes, QObject *parent = nullptr);

    bool open(OpenMode mode) override;
    bool isSequential() const override
    {
        return true;
    }
    qint64 bytesAvailable() const override;

    // Assigns the next sequence number; dropped frames leave a gap the receiver
    // sees as loss
    void enqueue(quint64 timestampUs, const QByteArray &data);
    // Queue a last frame and close once everything was read
    void finish();

    quint64 framesQueued() const
    {
        return m_framesQueued;
    }
    quint64 framesDropped() const
    {
        return m_framesDropped;
    }
    qint64 queuedBytes() const
    {
        return m_queuedBytes;
    }

Q_SIGNALS:
    // The upload job opened us: the receiver is connected
    void opened();

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    void closeWhenDrained();
    void push(const AudioFrame &frame);

    const qint64 m_maxQueuedBytes;
    std::deque<QByteArray> m_frames;
    qint64 m_headOffset = 0; // bytes of m_frames.front() already read
    qint64 m_queuedBytes = 0;
    quint64 m_framesQueued = 0;
    quint64 m_framesDropped = 0;
    quint32 m_nextSequence = 0;
    bool m_finishing = false;
};

} // namespace AudioStream
