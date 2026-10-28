/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

#include "framequeue.h"

#include <cstring>

namespace AudioStream
{

FrameQueue::FrameQueue(qint64 maxQueuedBytes, QObject *parent)
    : QIODevice(parent)
    , m_maxQueuedBytes(maxQueuedBytes)
{
}

bool FrameQueue::open(OpenMode mode)
{
    // Unbuffered: QIODevice must not read ahead, frames in its buffer could not
    // be dropped anymore
    if (!QIODevice::open((mode & ~WriteOnly) | ReadOnly | Unbuffered)) {
        return false;
    }
    Q_EMIT opened();
    return true;
}

qint64 FrameQueue::bytesAvailable() const
{
    return m_queuedBytes - m_headOffset + QIODevice::bytesAvailable();
}

void FrameQueue::enqueue(quint64 timestampUs, const QByteArray &data)
{
    // Nothing is queued before the receiver connected, it would only add latency
    if (m_finishing || !isOpen()) {
        return;
    }
    AudioFrame frame;
    frame.sequence = m_nextSequence++;
    frame.timestampUs = timestampUs;
    frame.data = data;
    push(frame);
}

void FrameQueue::push(const AudioFrame &frame)
{
    QByteArray encoded = FrameCodec::encode(frame);
    m_queuedBytes += encoded.size();
    m_frames.push_back(std::move(encoded));
    ++m_framesQueued;

    // Drop whole frames from the front, but never the one that is partially read
    while (m_queuedBytes > m_maxQueuedBytes && m_frames.size() > 1) {
        auto victim = m_frames.begin();
        if (m_headOffset > 0) {
            ++victim;
            if (victim == m_frames.end() || std::next(victim) == m_frames.end()) {
                break;
            }
        }
        m_queuedBytes -= victim->size();
        m_frames.erase(victim);
        ++m_framesDropped;
    }

    if (isOpen()) {
        Q_EMIT readyRead();
    }
}

void FrameQueue::finish()
{
    if (m_finishing) {
        return;
    }
    if (!isOpen()) {
        m_finishing = true;
        return;
    }
    AudioFrame last;
    last.last = true;
    last.sequence = m_nextSequence++;
    push(last);
    m_finishing = true;
    closeWhenDrained();
}

void FrameQueue::closeWhenDrained()
{
    if (m_finishing && m_frames.empty()) {
        // Deferred: the reader is usually in the middle of read() when we get here
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (isOpen()) {
                    close();
                }
            },
            Qt::QueuedConnection);
    }
}

qint64 FrameQueue::readData(char *data, qint64 maxSize)
{
    qint64 copied = 0;
    while (copied < maxSize && !m_frames.empty()) {
        const QByteArray &head = m_frames.front();
        const qint64 chunk = qMin(maxSize - copied, head.size() - m_headOffset);
        memcpy(data + copied, head.constData() + m_headOffset, chunk);
        copied += chunk;
        m_headOffset += chunk;
        if (m_headOffset == head.size()) {
            m_queuedBytes -= head.size();
            m_frames.pop_front();
            m_headOffset = 0;
        }
    }
    closeWhenDrained();
    return copied;
}

qint64 FrameQueue::writeData(const char *, qint64)
{
    return -1;
}

} // namespace AudioStream

#include "moc_framequeue.cpp"
