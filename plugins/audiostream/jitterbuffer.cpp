/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jitterbuffer.h"

#include <QtGlobal>

namespace AudioStream
{

namespace
{
constexpr int OverflowHeadroomMs = 200;

int ceilDiv(int a, int b)
{
    return (a + b - 1) / b;
}
} // namespace

JitterBuffer::JitterBuffer(int targetFrames, int maxFrames)
    : m_target(qMax(1, targetFrames))
    , m_max(qMax(m_target + 1, maxFrames))
{
}

int JitterBuffer::targetFramesFor(int latencyMs, int frameDurationMs)
{
    return qMax(1, ceilDiv(latencyMs, frameDurationMs));
}

int JitterBuffer::maxFramesFor(int latencyMs, int frameDurationMs)
{
    return targetFramesFor(latencyMs, frameDurationMs) + ceilDiv(OverflowHeadroomMs, frameDurationMs);
}

qint64 JitterBuffer::unwrap(quint32 sequence)
{
    if (m_highest < 0) {
        m_highest = sequence;
        return sequence;
    }
    // The distance to the newest frame we know of, taking the 32 bit wrap-around
    // into account
    const qint32 delta = qint32(sequence - quint32(m_highest));
    const qint64 unwrapped = m_highest + delta;
    if (unwrapped > m_highest) {
        m_highest = unwrapped;
    }
    return unwrapped;
}

void JitterBuffer::push(AudioFrame frame)
{
    const qint64 key = unwrap(frame.sequence);
    if (m_next >= 0 && key < m_next) {
        ++m_stats.late;
        return;
    }
    if (m_frames.count(key)) {
        ++m_stats.duplicates;
        return;
    }
    m_frames.emplace(key, std::move(frame));
    ++m_stats.received;

    if (int(m_frames.size()) > m_max) {
        while (int(m_frames.size()) > m_target) {
            m_frames.erase(m_frames.begin());
            ++m_stats.overflowDropped;
        }
        if (m_state == State::Playing) {
            m_next = m_frames.begin()->first;
        }
    }
}

JitterBuffer::Output JitterBuffer::pop()
{
    Output out;
    if (m_state == State::Buffering) {
        if (int(m_frames.size()) < m_target) {
            return out;
        }
        const qint64 first = m_frames.begin()->first;
        if (m_next >= 0 && first > m_next) {
            // Frames that never arrived while we were rebuffering
            m_stats.lost += quint64(first - m_next);
        }
        m_next = first;
        m_state = State::Playing;
    }

    if (m_frames.empty()) {
        ++m_stats.underruns;
        m_state = State::Buffering;
        return out;
    }

    auto it = m_frames.begin();
    if (it->first == m_next) {
        out.kind = Output::Frame;
        out.frame = std::move(it->second);
        m_frames.erase(it);
        ++m_stats.played;
    } else {
        out.kind = Output::Concealed;
        ++m_stats.lost;
    }
    ++m_next;
    return out;
}

} // namespace AudioStream
