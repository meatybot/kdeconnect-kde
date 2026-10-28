/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <optional>

namespace AudioStream
{

// One frame of the audiostream payload, see docs/specs/audiostream.md "Framing"
struct AudioFrame {
    quint32 sequence = 0;
    quint64 timestampUs = 0;
    bool last = false;
    QByteArray data;
};

namespace FrameCodec
{
constexpr int HeaderSize = 20;
constexpr quint32 MaxDataSize = 65536;
constexpr quint8 Version = 1;
constexpr quint8 FlagLast = 0x01;

void encodeInto(QByteArray &out, const AudioFrame &frame);
QByteArray encode(const AudioFrame &frame);
} // namespace FrameCodec

// Incremental decoder: feed it whatever arrives from the payload and pull
// complete frames. Garbage (bad magic, unknown version, oversized length) is
// skipped byte by byte until a valid header is found again.
class FrameDecoder
{
public:
    void feed(const QByteArray &bytes);
    std::optional<AudioFrame> next();

    qint64 discardedBytes() const
    {
        return m_discarded;
    }
    qsizetype pendingBytes() const
    {
        return m_buffer.size() - m_pos;
    }

private:
    void compact();

    QByteArray m_buffer;
    qsizetype m_pos = 0;
    qint64 m_discarded = 0;
};

} // namespace AudioStream
