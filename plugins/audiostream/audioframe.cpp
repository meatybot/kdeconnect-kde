/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "audioframe.h"

#include <QtEndian>

namespace AudioStream
{

namespace
{
constexpr char Magic0 = 'K';
constexpr char Magic1 = 'A';
// Don't keep consumed bytes around forever, but don't memmove on every frame
// either
constexpr qsizetype CompactThreshold = 64 * 1024;
} // namespace

void FrameCodec::encodeInto(QByteArray &out, const AudioFrame &frame)
{
    Q_ASSERT(quint32(frame.data.size()) <= MaxDataSize);
    const qsizetype start = out.size();
    out.resize(start + HeaderSize + frame.data.size());
    uchar *p = reinterpret_cast<uchar *>(out.data()) + start;
    p[0] = Magic0;
    p[1] = Magic1;
    p[2] = Version;
    p[3] = frame.last ? FlagLast : 0;
    qToBigEndian<quint32>(frame.sequence, p + 4);
    qToBigEndian<quint64>(frame.timestampUs, p + 8);
    qToBigEndian<quint32>(quint32(frame.data.size()), p + 16);
    if (!frame.data.isEmpty()) {
        memcpy(p + HeaderSize, frame.data.constData(), frame.data.size());
    }
}

QByteArray FrameCodec::encode(const AudioFrame &frame)
{
    QByteArray out;
    out.reserve(HeaderSize + frame.data.size());
    encodeInto(out, frame);
    return out;
}

void FrameDecoder::feed(const QByteArray &bytes)
{
    m_buffer.append(bytes);
}

void FrameDecoder::compact()
{
    if (m_pos == m_buffer.size()) {
        m_buffer.clear();
        m_pos = 0;
    } else if (m_pos > CompactThreshold) {
        m_buffer.remove(0, m_pos);
        m_pos = 0;
    }
}

std::optional<AudioFrame> FrameDecoder::next()
{
    while (m_buffer.size() - m_pos >= FrameCodec::HeaderSize) {
        const uchar *p = reinterpret_cast<const uchar *>(m_buffer.constData()) + m_pos;
        const quint32 length = qFromBigEndian<quint32>(p + 16);
        if (p[0] != uchar(Magic0) || p[1] != uchar(Magic1) || p[2] != FrameCodec::Version || length > FrameCodec::MaxDataSize) {
            ++m_pos;
            ++m_discarded;
            continue;
        }
        if (m_buffer.size() - m_pos < FrameCodec::HeaderSize + qsizetype(length)) {
            break;
        }
        AudioFrame frame;
        frame.last = (p[3] & FrameCodec::FlagLast) != 0;
        frame.sequence = qFromBigEndian<quint32>(p + 4);
        frame.timestampUs = qFromBigEndian<quint64>(p + 8);
        frame.data = m_buffer.mid(m_pos + FrameCodec::HeaderSize, length);
        m_pos += FrameCodec::HeaderSize + length;
        compact();
        return frame;
    }
    compact();
    return std::nullopt;
}

} // namespace AudioStream
