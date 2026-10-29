/**
 * SPDX-FileCopyrightText: 2026 meatybot <avatarcc@tuta.io>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "audiostreamplugin.h"

#include "audiostreamsession.h"
#include "plugin_audiostream_debug.h"
#include "pulsecapture.h"

#include <KLocalizedString>
#include <KNotification>
#include <KPluginFactory>

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <core/device.h>
#include <core/daemon.h>

K_PLUGIN_CLASS_WITH_JSON(AudioStreamPlugin, "kdeconnect_audiostream.json")

using namespace AudioStream;

namespace
{
// The peer may have to wait for its user to accept a consent dialog
constexpr int RequestTimeoutMs = 60000;
// How long the local user has to allow a remote microphone/call request
constexpr int ConfirmationTimeoutMs = 30000;
// CompositeUploadJob gives up after 30 s without the receiver connecting
constexpr int ConnectTimeoutMs = 35000;

QString sourceName(const QString &source)
{
    if (source == Source::Media) {
        return i18nc("audio source", "media");
    } else if (source == Source::System) {
        return i18nc("audio source", "system");
    } else if (source == Source::Call) {
        return i18nc("audio source", "call");
    } else if (source == Source::Microphone) {
        return i18nc("audio source", "microphone");
    }
    return source;
}

// What to tell the user when the device ends a stream we receive, empty when nothing went wrong
QString remoteStopReason(const QString &error, const QString &source)
{
    if (error == Error::Silenced) {
        return source == Source::Call ? i18n("The device mutes its microphone during phone calls, so the call audio cannot be streamed")
                                      : i18n("The device stopped sending audio because its system muted the recording");
    } else if (error == Error::PermissionDenied) {
        return i18n("The device did not allow the audio stream");
    } else if (error == Error::CaptureFailed) {
        return i18n("The device could not record its audio");
    } else if (error == Error::Timeout) {
        return i18n("The device did not answer the audio stream request");
    }
    return {};
}
} // namespace

AudioStreamPlugin::AudioStreamPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    m_localCapabilities.sources = Source::all();
}

AudioStreamPlugin::~AudioStreamPlugin()
{
    const QStringList ids = m_sessions.keys();
    for (const QString &id : ids) {
        endSession(id, {}, true);
    }
}

QString AudioStreamPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/audiostream").arg(device()->id());
}

void AudioStreamPlugin::connected()
{
    NetworkPacket request = makeCapabilitiesRequest();
    sendPacket(request);
    NetworkPacket caps = makeCapabilitiesPacket(m_localCapabilities);
    sendPacket(caps);
}

QString AudioStreamPlugin::startStream(const QString &source)
{
    if (!Source::isValid(source) || !m_localCapabilities.sources.contains(source)) {
        qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Unsupported source" << source;
        return {};
    }
    if (m_sessions.size() >= m_localCapabilities.maxStreams) {
        qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Too many streams";
        return {};
    }
    const QString streamId = newStreamId();
    StreamFormat format;
    format.channels = PulseCapture::sourceChannels(source);
    startOutgoing(streamId, source, format);
    return m_sessions.contains(streamId) ? streamId : QString();
}

QString AudioStreamPlugin::requestStream(const QString &source)
{
    if (!Source::isValid(source)) {
        return {};
    }
    if (m_sessions.size() >= m_localCapabilities.maxStreams) {
        qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Too many streams";
        return {};
    }
    const QString streamId = newStreamId();
    StreamFormat preferred;
    preferred.channels = (source == Source::Media || source == Source::System) ? 2 : 1;
    NetworkPacket np = makeStartRequest(streamId, source, preferred);
    if (!sendPacket(np)) {
        return {};
    }
    Session &session = m_sessions[streamId];
    session.source = source;
    session.sending = false;
    session.state = State::Requested;
    session.format = preferred;
    armTimeout(streamId, RequestTimeoutMs);
    return streamId;
}

void AudioStreamPlugin::stopStream(const QString &streamId)
{
    endSession(streamId, {}, true);
}

void AudioStreamPlugin::stopAllStreams()
{
    const QStringList ids = m_sessions.keys();
    for (const QString &id : ids) {
        endSession(id, {}, true);
    }
}

QStringList AudioStreamPlugin::activeStreams() const
{
    return m_sessions.keys();
}

QVariantMap AudioStreamPlugin::streamInfo(const QString &streamId) const
{
    const auto it = m_sessions.constFind(streamId);
    if (it == m_sessions.constEnd()) {
        return {};
    }
    const Session &s = *it;
    QString state;
    switch (s.state) {
    case State::Requested:
        state = QStringLiteral("requested");
        break;
    case State::AwaitingConfirmation:
        state = QStringLiteral("awaitingConfirmation");
        break;
    case State::Active:
        state = QStringLiteral("active");
        break;
    }
    QVariantMap info{
        {QStringLiteral("direction"), s.sending ? QStringLiteral("send") : QStringLiteral("receive")},
        {QStringLiteral("state"), state},
        {QStringLiteral("source"), s.source},
        {QStringLiteral("codec"), s.format.codec},
        {QStringLiteral("sampleRate"), s.format.sampleRate},
        {QStringLiteral("channels"), s.format.channels},
    };
    if (s.outgoing) {
        info[QStringLiteral("framesSent")] = s.outgoing->payload()->framesQueued() - s.outgoing->payload()->framesDropped();
        info[QStringLiteral("framesDropped")] = s.outgoing->payload()->framesDropped();
    }
    if (s.incoming) {
        const JitterBuffer::Stats stats = s.incoming->playbackDevice()->stats();
        info[QStringLiteral("framesReceived")] = s.incoming->framesDecoded();
        info[QStringLiteral("framesLost")] = stats.lost;
        info[QStringLiteral("framesLate")] = stats.late;
        info[QStringLiteral("underruns")] = stats.underruns;
        info[QStringLiteral("bufferedMs")] = s.incoming->bufferedMs();
        info[QStringLiteral("jitterMs")] = s.incoming->jitterMs();
    }
    return info;
}

void AudioStreamPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_AUDIOSTREAM) {
        handleStreamPacket(np);
    } else if (np.type() == PACKET_TYPE_AUDIOSTREAM_REQUEST) {
        handleRequestPacket(np);
    }
}

void AudioStreamPlugin::handleStreamPacket(const NetworkPacket &np)
{
    Message m;
    const QString error = parseMessage(np, &m);

    if (m.action == Action::Capabilities && error.isEmpty()) {
        m_remoteCapabilities = parseCapabilities(np);
        Q_EMIT remoteSourcesChanged();
        return;
    }

    if (m.action == Action::Stop && error.isEmpty()) {
        const auto it = m_sessions.constFind(m.streamId);
        const bool receiving = it != m_sessions.constEnd() && !it->sending;
        // The packet and the payload travel on different sockets: the stream may already have
        // ended through its last frame, the reason is still worth telling
        if (receiving || it == m_sessions.constEnd()) {
            const QString reason = remoteStopReason(m.error, receiving ? it->source : m.source);
            if (!reason.isEmpty()) {
                Daemon::instance()->sendSimpleNotification(QStringLiteral("audioStream"), device()->name(), reason, QStringLiteral("dialog-warning"));
            }
        }
        if (receiving) {
            endSession(m.streamId, m.error, false);
        }
        return;
    }

    // Anything else carrying a payload is a start packet, we have to close it if
    // we refuse it
    auto refuse = [&](const QString &reason) {
        qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Refusing stream" << m.streamId << reason;
        if (np.payload()) {
            np.payload()->close();
        }
        if (isValidStreamId(m.streamId)) {
            sendStop(PACKET_TYPE_AUDIOSTREAM_REQUEST, m.streamId, m.source, reason);
        }
    };

    if (!error.isEmpty()) {
        refuse(error);
        // A request of ours that was answered with a format we can't play
        if (m_sessions.contains(m.streamId) && !m_sessions[m.streamId].sending) {
            endSession(m.streamId, error, false);
        }
        return;
    }
    if (m.action != Action::Start) {
        return;
    }
    if (!np.payload() || np.payloadSize() != -1) {
        refuse(Error::InvalidRequest);
        return;
    }

    const auto it = m_sessions.find(m.streamId);
    if (it != m_sessions.end()) {
        if (it->sending || it->state != State::Requested) {
            refuse(Error::Busy);
            return;
        }
        // Answer to one of our requests
        startIncoming(m.streamId, m, np.payload());
        return;
    }

    const QString reason = checkIncomingStart(m, m_localCapabilities, m_sessions.size(), false);
    if (!reason.isEmpty()) {
        refuse(reason);
        return;
    }
    startIncoming(m.streamId, m, np.payload());
}

void AudioStreamPlugin::handleRequestPacket(const NetworkPacket &np)
{
    Message m;
    const QString error = parseMessage(np, &m);

    if (m.action == Action::Capabilities && error.isEmpty()) {
        NetworkPacket caps = makeCapabilitiesPacket(m_localCapabilities);
        sendPacket(caps);
        return;
    }

    if (m.action == Action::Stop && error.isEmpty()) {
        const auto it = m_sessions.constFind(m.streamId);
        if (it == m_sessions.constEnd()) {
            return;
        }
        // The receiver stopped or rejected the stream: confirm with a stop of our
        // own if we send
        endSession(m.streamId, m.error, it->sending);
        return;
    }

    if (!error.isEmpty()) {
        if (isValidStreamId(m.streamId)) {
            sendStop(PACKET_TYPE_AUDIOSTREAM, m.streamId, m.source, error);
        }
        return;
    }
    if (m.action != Action::Start) {
        return;
    }

    const Decision decision =
        decideStartRequest(m, m_localCapabilities, m_sessions.size(), m_sessions.contains(m.streamId), PulseCapture::sourceChannels(m.source));
    if (!decision.error.isEmpty()) {
        sendStop(PACKET_TYPE_AUDIOSTREAM, m.streamId, m.source, decision.error);
        return;
    }

    if (m.source == Source::Microphone || m.source == Source::Call) {
        askConfirmation(m.streamId, m, decision.format);
    } else {
        startOutgoing(m.streamId, m.source, decision.format);
    }
}

void AudioStreamPlugin::askConfirmation(const QString &streamId, const Message &request, const StreamFormat &format)
{
    Session &session = m_sessions[streamId];
    session.source = request.source;
    session.sending = true;
    session.state = State::AwaitingConfirmation;
    session.format = format;

    KNotification *notification = new KNotification(QStringLiteral("audioStreamRequest"), KNotification::Persistent);
    notification->setComponentName(QStringLiteral("kdeconnect"));
    notification->setIconName(QStringLiteral("audio-input-microphone"));
    notification->setTitle(device()->name());
    notification->setText(i18n("%1 wants to receive your %2 audio", device()->name(), sourceName(request.source)));
    KNotificationAction *allow = notification->addAction(i18nc("@action:button allow audio streaming", "Allow"));
    KNotificationAction *deny = notification->addAction(i18nc("@action:button deny audio streaming", "Deny"));
    connect(allow, &KNotificationAction::activated, this, [this, streamId] {
        const auto it = m_sessions.find(streamId);
        if (it == m_sessions.end() || it->state != State::AwaitingConfirmation) {
            return;
        }
        const QString source = it->source;
        const StreamFormat format = it->format;
        if (it->timeout) {
            it->timeout->deleteLater();
        }
        m_sessions.erase(it);
        startOutgoing(streamId, source, format);
    });
    connect(deny, &KNotificationAction::activated, this, [this, streamId] {
        endSession(streamId, Error::PermissionDenied, true);
    });
    connect(notification, &KNotification::closed, this, [this, streamId] {
        const auto it = m_sessions.constFind(streamId);
        if (it != m_sessions.constEnd() && it->state == State::AwaitingConfirmation) {
            endSession(streamId, Error::PermissionDenied, true);
        }
    });
    session.notification = notification;
    armTimeout(streamId, ConfirmationTimeoutMs);
    notification->sendEvent();
}

void AudioStreamPlugin::startOutgoing(const QString &streamId, const QString &source, const StreamFormat &format)
{
    auto *capture = new PulseCapture(source, format.channels);
    auto *stream = new OutgoingStream(streamId, source, capture, this);
    const StreamFormat actual = stream->format();

    NetworkPacket np = makeStartPacket(streamId, source, actual, source == Source::Call ? QStringLiteral("full") : QString());
    np.setPayload(stream->payload(), -1);

    Session &session = m_sessions[streamId];
    session.source = source;
    session.sending = true;
    session.state = State::Active;
    session.format = actual;
    session.outgoing = stream;

    connect(stream, &OutgoingStream::connected, this, [this, streamId, stream] {
        const auto it = m_sessions.find(streamId);
        if (it != m_sessions.end() && it->timeout) {
            it->timeout->deleteLater();
        }
        stream->start();
    });
    connect(stream, &OutgoingStream::finished, this, [this, streamId](const QString &error) {
        endSession(streamId, error, true);
    });

    if (!sendPacket(np)) {
        endSession(streamId, Error::CaptureFailed, false);
        return;
    }
    armTimeout(streamId, ConnectTimeoutMs);
    showActiveNotification(streamId);
    Q_EMIT streamStarted(streamId);
}

void AudioStreamPlugin::startIncoming(const QString &streamId, const Message &start, const QSharedPointer<QIODevice> &payload)
{
    Session &session = m_sessions[streamId];
    if (session.timeout) {
        session.timeout->deleteLater();
    }
    session.source = start.source;
    session.sending = false;
    session.state = State::Active;
    session.format = start.format;

    QAudioFormat format;
    format.setSampleRate(start.format.sampleRate);
    format.setChannelCount(start.format.channels);
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull() || !output.isFormatSupported(format)) {
        qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "No audio output for" << format;
        payload->close();
        endSession(streamId, Error::PlaybackFailed, true);
        return;
    }

    auto *stream = new IncomingStream(streamId, start.source, start.format, payload, DefaultLatencyMs, this);
    auto *sink = new QAudioSink(output, format, this);
    // Two frames of device buffer: the jitter buffer is where latency is managed
    sink->setBufferSize(start.format.frameBytes() * 2);
    session.incoming = stream;
    session.sink = sink;

    connect(stream, &IncomingStream::finished, this, [this, streamId](const QString &error) {
        endSession(streamId, error, false);
    });
    connect(sink, &QAudioSink::stateChanged, this, [this, streamId, sink](QAudio::State state) {
        if (state == QAudio::StoppedState && sink->error() != QAudio::NoError) {
            qCWarning(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Playback failed" << sink->error();
            endSession(streamId, Error::PlaybackFailed, true);
        }
    });

    stream->start();
    if (!m_sessions.contains(streamId)) {
        return; // the payload was already finished
    }
    sink->start(stream->playbackDevice());
    if (!m_sessions.contains(streamId)) {
        return;
    }
    showActiveNotification(streamId);
    Q_EMIT streamStarted(streamId);
}

void AudioStreamPlugin::showActiveNotification(const QString &streamId)
{
    Session &session = m_sessions[streamId];
    KNotification *notification = new KNotification(QStringLiteral("audioStream"), KNotification::Persistent);
    notification->setComponentName(QStringLiteral("kdeconnect"));
    notification->setTitle(device()->name());
    if (session.sending) {
        notification->setIconName(session.source == Source::Microphone || session.source == Source::Call ? QStringLiteral("audio-input-microphone")
                                                                                                         : QStringLiteral("audio-volume-high"));
        notification->setText(i18n("Sending %1 audio to %2", sourceName(session.source), device()->name()));
    } else {
        notification->setIconName(QStringLiteral("audio-speakers-symbolic"));
        notification->setText(i18n("Playing %1 audio from %2", sourceName(session.source), device()->name()));
    }
    KNotificationAction *stop = notification->addAction(i18nc("@action:button stop audio streaming", "Stop"));
    connect(stop, &KNotificationAction::activated, this, [this, streamId] {
        endSession(streamId, {}, true);
    });
    session.notification = notification;
    notification->sendEvent();
}

void AudioStreamPlugin::armTimeout(const QString &streamId, int ms)
{
    Session &session = m_sessions[streamId];
    if (session.timeout) {
        session.timeout->deleteLater();
    }
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, streamId] {
        const auto it = m_sessions.constFind(streamId);
        if (it == m_sessions.constEnd()) {
            return;
        }
        // Not answering a confirmation means no
        endSession(streamId, it->state == State::AwaitingConfirmation ? Error::PermissionDenied : Error::Timeout, true);
    });
    timer->start(ms);
    session.timeout = timer;
}

void AudioStreamPlugin::sendStop(const QString &type, const QString &streamId, const QString &source, const QString &error)
{
    NetworkPacket np = makeStopPacket(type, streamId, source, error);
    sendPacket(np);
}

void AudioStreamPlugin::endSession(const QString &streamId, const QString &error, bool notifyPeer)
{
    const auto it = m_sessions.find(streamId);
    if (it == m_sessions.end()) {
        return;
    }
    Session session = *it;
    m_sessions.erase(it);

    if (session.timeout) {
        session.timeout->deleteLater();
    }
    if (session.notification) {
        session.notification->disconnect(this);
        session.notification->close();
    }
    if (session.outgoing) {
        session.outgoing->disconnect(this);
        if (error.isEmpty() && !session.outgoing->isEnded() && session.outgoing->payload()->isOpen()) {
            // Let the receiver get what was captured and the last frame
            connect(session.outgoing.data(), &OutgoingStream::finished, session.outgoing.data(), &QObject::deleteLater);
            session.outgoing->finish();
        } else {
            session.outgoing->deleteLater();
        }
    }
    if (session.sink) {
        session.sink->disconnect(this);
        session.sink->stop();
        session.sink->deleteLater();
    }
    if (session.incoming) {
        session.incoming->disconnect(this);
        session.incoming->close();
        session.incoming->deleteLater();
    }
    if (notifyPeer) {
        sendStop(session.sending ? PACKET_TYPE_AUDIOSTREAM : PACKET_TYPE_AUDIOSTREAM_REQUEST, streamId, session.source, error);
    }
    qCDebug(KDECONNECT_PLUGIN_AUDIOSTREAM) << "Stream" << streamId << "ended" << error;
    Q_EMIT streamStopped(streamId, error);
}

#include "audiostreamplugin.moc"
#include "moc_audiostreamplugin.cpp"
