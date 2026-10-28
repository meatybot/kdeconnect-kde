/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "audioframe.h"

#include <map>

namespace AudioStream
{

// Reorders frames by sequence number and paces them out at the rate the audio
// output asks for. See docs/specs/audiostream.md "Jitter buffer and latency
// target".
class JitterBuffer
{
public:
    enum class State {
        Buffering,
        Playing,
    };

    struct Stats {
        quint64 received = 0;
        quint64 played = 0;
        quint64 lost = 0;
        quint64 late = 0;
        quint64 duplicates = 0;
        quint64 overflowDropped = 0;
        quint64 underruns = 0;
    };

    struct Output {
        enum Kind {
            Frame, // a real frame, in `frame`
            Concealed, // the next frame is missing: play silence in its place
            Silence, // nothing to play yet (buffering)
        };
        Kind kind = Silence;
        AudioFrame frame;
    };

    // targetFrames: frames to prefill before playing; maxFrames: depth at which
    // the oldest frames get dropped back down to targetFrames
    JitterBuffer(int targetFrames, int maxFrames);

    void push(AudioFrame frame);
    Output pop();

    int depth() const
    {
        return int(m_frames.size());
    }
    State state() const
    {
        return m_state;
    }
    const Stats &stats() const
    {
        return m_stats;
    }
    int targetFrames() const
    {
        return m_target;
    }

    // Frame counts for a latency target, see the spec
    static int targetFramesFor(int latencyMs, int frameDurationMs);
    static int maxFramesFor(int latencyMs, int frameDurationMs);

private:
    qint64 unwrap(quint32 sequence);

    const int m_target;
    const int m_max;
    std::map<qint64, AudioFrame> m_frames;
    State m_state = State::Buffering;
    qint64 m_next = -1; // unwrapped sequence of the next frame to play, -1 before
                        // the first one
    qint64 m_highest = -1; // highest unwrapped sequence seen, the reference to unwrap new ones
    Stats m_stats;
};

} // namespace AudioStream
