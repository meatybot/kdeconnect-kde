/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QString>
#include <QStringList>

#include <core/networkpacket.h>

#define PACKET_TYPE_AUDIOSTREAM QStringLiteral("kdeconnect.audiostream")
#define PACKET_TYPE_AUDIOSTREAM_REQUEST QStringLiteral("kdeconnect.audiostream.request")

// Packet building, parsing and negotiation, see docs/specs/audiostream.md
namespace AudioStream
{

namespace Action
{
inline const QString Start = QStringLiteral("start");
inline const QString Stop = QStringLiteral("stop");
inline const QString Capabilities = QStringLiteral("capabilities");
} // namespace Action

namespace Source
{
inline const QString Media = QStringLiteral("media");
inline const QString System = QStringLiteral("system");
inline const QString Call = QStringLiteral("call");
inline const QString Microphone = QStringLiteral("microphone");
QStringList all();
bool isValid(const QString &source);
} // namespace Source

namespace Codec
{
inline const QString PcmS16le = QStringLiteral("pcm_s16le");
}

namespace Error
{
inline const QString UnsupportedSource = QStringLiteral("unsupported_source");
inline const QString UnsupportedFormat = QStringLiteral("unsupported_format");
inline const QString PermissionDenied = QStringLiteral("permission_denied");
inline const QString Busy = QStringLiteral("busy");
inline const QString CaptureFailed = QStringLiteral("capture_failed");
inline const QString PlaybackFailed = QStringLiteral("playback_failed");
inline const QString Timeout = QStringLiteral("timeout");
inline const QString InvalidRequest = QStringLiteral("invalid_request");
} // namespace Error

constexpr int ProtocolVersion = 1;
constexpr int DefaultSampleRate = 48000;
constexpr int DefaultFrameDurationMs = 20;
constexpr int MaxStreams = 4;

struct StreamFormat {
    QString codec = Codec::PcmS16le;
    int sampleRate = DefaultSampleRate;
    int channels = 2;
    int frameDurationMs = DefaultFrameDurationMs;

    int bytesPerSample() const
    {
        return 2;
    }
    int frameBytes() const
    {
        return sampleRate * frameDurationMs / 1000 * channels * bytesPerSample();
    }
    bool operator==(const StreamFormat &other) const = default;
};

struct Message {
    QString action;
    QString streamId;
    QString source;
    StreamFormat format;
    QString error; // stop only, empty for a normal stop
    QString callAudio; // start with source "call" only
};

struct Capabilities {
    QStringList sources;
    QStringList codecs = {Codec::PcmS16le};
    QList<int> sampleRates = {DefaultSampleRate};
    bool canPlay = true;
    int maxStreams = MaxStreams;
};

bool isValidStreamId(const QString &streamId);
QString newStreamId();

// Returns an empty string when the packet is valid, otherwise
// Error::InvalidRequest or Error::UnsupportedFormat. Works for both packet
// types.
QString parseMessage(const NetworkPacket &np, Message *out);
Capabilities parseCapabilities(const NetworkPacket &np);

NetworkPacket makeStartPacket(const QString &streamId, const QString &source, const StreamFormat &format, const QString &callAudio = {});
NetworkPacket makeStopPacket(const QString &type, const QString &streamId, const QString &source = {}, const QString &error = {});
NetworkPacket makeStartRequest(const QString &streamId, const QString &source, const StreamFormat &preferred);
NetworkPacket makeCapabilitiesRequest();
NetworkPacket makeCapabilitiesPacket(const Capabilities &caps);

// Format the sender will use for a request: the requested one if we support it,
// else our default
StreamFormat negotiateFormat(const StreamFormat &requested, int sourceChannels);

struct Decision {
    QString error; // empty when accepted
    StreamFormat format;
};

// What a sender answers to a start request
Decision decideStartRequest(const Message &request, const Capabilities &local, int activeStreams, bool streamIdInUse, int sourceChannels);

// Whether a receiver can play an incoming start packet (empty when it can)
QString checkIncomingStart(const Message &start, const Capabilities &local, int activeStreams, bool streamIdInUse);

} // namespace AudioStream
