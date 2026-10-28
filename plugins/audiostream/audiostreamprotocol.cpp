/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

#include "audiostreamprotocol.h"

#include <QRegularExpression>
#include <QUuid>

namespace AudioStream
{

namespace
{
const QString KeyAction = QStringLiteral("action");
const QString KeyStreamId = QStringLiteral("streamId");
const QString KeySource = QStringLiteral("source");
const QString KeyCodec = QStringLiteral("codec");
const QString KeySampleRate = QStringLiteral("sampleRate");
const QString KeyChannels = QStringLiteral("channels");
const QString KeyFrameDuration = QStringLiteral("frameDurationMs");
const QString KeyError = QStringLiteral("error");
const QString KeyCallAudio = QStringLiteral("callAudio");

bool isNumber(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    case QMetaType::Double:
        return v.toDouble() == double(v.toLongLong());
    default:
        return false;
    }
}

// Reads an optional integer field: absent -> fallback, present but invalid ->
// false
bool readInt(const QVariantMap &body, const QString &key, int min, int max, int fallback, int *out)
{
    const auto it = body.constFind(key);
    if (it == body.constEnd()) {
        *out = fallback;
        return true;
    }
    if (!isNumber(*it)) {
        return false;
    }
    const qlonglong value = it->toLongLong();
    if (value < min || value > max) {
        return false;
    }
    *out = int(value);
    return true;
}
} // namespace

QStringList Source::all()
{
    return {Media, System, Call, Microphone};
}

bool Source::isValid(const QString &source)
{
    return all().contains(source);
}

bool isValidStreamId(const QString &streamId)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:-]{1,64}$"));
    return re.match(streamId).hasMatch();
}

QString newStreamId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString parseMessage(const NetworkPacket &np, Message *out)
{
    const QVariantMap body = np.body();
    Message m;
    m.action = body.value(KeyAction).toString();
    if (m.action != Action::Start && m.action != Action::Stop && m.action != Action::Capabilities) {
        return Error::InvalidRequest;
    }
    m.error = body.value(KeyError).toString();
    m.callAudio = body.value(KeyCallAudio).toString();
    m.source = body.value(KeySource).toString();

    if (m.action == Action::Capabilities) {
        *out = m;
        return {};
    }

    m.streamId = body.value(KeyStreamId).toString();
    if (!isValidStreamId(m.streamId)) {
        return Error::InvalidRequest;
    }
    if (m.action == Action::Stop) {
        *out = m;
        return {};
    }

    if (!Source::isValid(m.source)) {
        return Error::InvalidRequest;
    }
    if (!body.contains(KeyCodec) || !body.contains(KeySampleRate) || !body.contains(KeyChannels)) {
        return Error::InvalidRequest;
    }
    m.format.codec = body.value(KeyCodec).toString();
    if (!readInt(body, KeySampleRate, 8000, 96000, DefaultSampleRate, &m.format.sampleRate) || !readInt(body, KeyChannels, 1, 2, 2, &m.format.channels)
        || !readInt(body, KeyFrameDuration, 5, 100, DefaultFrameDurationMs, &m.format.frameDurationMs)) {
        return Error::InvalidRequest;
    }
    *out = m;
    if (m.format.codec != Codec::PcmS16le) {
        return Error::UnsupportedFormat;
    }
    return {};
}

Capabilities parseCapabilities(const NetworkPacket &np)
{
    const QVariantMap body = np.body();
    Capabilities caps;
    caps.sources.clear();
    const QStringList sources = body.value(QStringLiteral("sources")).toStringList();
    for (const QString &s : sources) {
        if (Source::isValid(s) && !caps.sources.contains(s)) {
            caps.sources.append(s);
        }
    }
    if (body.contains(QStringLiteral("codecs"))) {
        caps.codecs = body.value(QStringLiteral("codecs")).toStringList();
    }
    if (body.contains(QStringLiteral("sampleRates"))) {
        caps.sampleRates.clear();
        const QVariantList rates = body.value(QStringLiteral("sampleRates")).toList();
        for (const QVariant &r : rates) {
            if (isNumber(r)) {
                caps.sampleRates.append(r.toInt());
            }
        }
    }
    caps.canPlay = body.value(QStringLiteral("canPlay"), true).toBool();
    int maxStreams = MaxStreams;
    if (readInt(body, QStringLiteral("maxStreams"), 0, 64, MaxStreams, &maxStreams)) {
        caps.maxStreams = maxStreams;
    }
    return caps;
}

NetworkPacket makeStartPacket(const QString &streamId, const QString &source, const StreamFormat &format, const QString &callAudio)
{
    NetworkPacket np(PACKET_TYPE_AUDIOSTREAM,
                     {
                         {KeyAction, Action::Start},
                         {KeyStreamId, streamId},
                         {KeySource, source},
                         {KeyCodec, format.codec},
                         {KeySampleRate, format.sampleRate},
                         {KeyChannels, format.channels},
                         {KeyFrameDuration, format.frameDurationMs},
                     });
    if (!callAudio.isEmpty()) {
        np.set(KeyCallAudio, callAudio);
    }
    return np;
}

NetworkPacket makeStopPacket(const QString &type, const QString &streamId, const QString &source, const QString &error)
{
    NetworkPacket np(type, {{KeyAction, Action::Stop}, {KeyStreamId, streamId}});
    if (!source.isEmpty()) {
        np.set(KeySource, source);
    }
    if (!error.isEmpty()) {
        np.set(KeyError, error);
    }
    return np;
}

NetworkPacket makeStartRequest(const QString &streamId, const QString &source, const StreamFormat &preferred)
{
    return NetworkPacket(PACKET_TYPE_AUDIOSTREAM_REQUEST,
                         {
                             {KeyAction, Action::Start},
                             {KeyStreamId, streamId},
                             {KeySource, source},
                             {KeyCodec, preferred.codec},
                             {KeySampleRate, preferred.sampleRate},
                             {KeyChannels, preferred.channels},
                         });
}

NetworkPacket makeCapabilitiesRequest()
{
    return NetworkPacket(PACKET_TYPE_AUDIOSTREAM_REQUEST, {{KeyAction, Action::Capabilities}});
}

NetworkPacket makeCapabilitiesPacket(const Capabilities &caps)
{
    QVariantList rates;
    for (int r : caps.sampleRates) {
        rates.append(r);
    }
    return NetworkPacket(PACKET_TYPE_AUDIOSTREAM,
                         {
                             {KeyAction, Action::Capabilities},
                             {QStringLiteral("protocolVersion"), ProtocolVersion},
                             {QStringLiteral("sources"), caps.sources},
                             {QStringLiteral("codecs"), caps.codecs},
                             {QStringLiteral("sampleRates"), rates},
                             {QStringLiteral("canPlay"), caps.canPlay},
                             {QStringLiteral("maxStreams"), caps.maxStreams},
                         });
}

StreamFormat negotiateFormat(const StreamFormat &requested, int sourceChannels)
{
    StreamFormat format;
    format.channels = qBound(1, sourceChannels, 2);
    if (requested.codec == Codec::PcmS16le && requested.sampleRate == DefaultSampleRate) {
        // Downmixing is cheap, upmixing a mono source to stereo is not worth the
        // bandwidth
        format.channels = qMin(format.channels, requested.channels);
    }
    return format;
}

Decision decideStartRequest(const Message &request, const Capabilities &local, int activeStreams, bool streamIdInUse, int sourceChannels)
{
    Decision d;
    if (!local.sources.contains(request.source)) {
        d.error = Error::UnsupportedSource;
    } else if (request.format.codec != Codec::PcmS16le) {
        d.error = Error::UnsupportedFormat;
    } else if (streamIdInUse || activeStreams >= local.maxStreams) {
        d.error = Error::Busy;
    } else {
        d.format = negotiateFormat(request.format, sourceChannels);
    }
    return d;
}

QString checkIncomingStart(const Message &start, const Capabilities &local, int activeStreams, bool streamIdInUse)
{
    if (!local.canPlay) {
        return Error::PlaybackFailed;
    }
    if (!local.codecs.contains(start.format.codec)) {
        return Error::UnsupportedFormat;
    }
    if (streamIdInUse || activeStreams >= local.maxStreams) {
        return Error::Busy;
    }
    return {};
}

} // namespace AudioStream
