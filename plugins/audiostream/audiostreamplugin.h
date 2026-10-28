/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include "audiostreamprotocol.h"

#include <core/kdeconnectplugin.h>

#include <QMap>
#include <QPointer>
#include <QTimer>

class KNotification;
class QAudioSink;

namespace AudioStream
{
class IncomingStream;
class OutgoingStream;
} // namespace AudioStream

class AudioStreamPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.audiostream")
    Q_PROPERTY(QStringList remoteSources READ remoteSources NOTIFY remoteSourcesChanged)

public:
    explicit AudioStreamPlugin(QObject *parent, const QVariantList &args);
    ~AudioStreamPlugin() override;

    // Send local audio of `source` to the device; returns the stream id, empty on
    // failure
    Q_SCRIPTABLE QString startStream(const QString &source);
    // Ask the device to send its `source` audio here; returns the stream id,
    // empty on failure
    Q_SCRIPTABLE QString requestStream(const QString &source);
    Q_SCRIPTABLE void stopStream(const QString &streamId);
    Q_SCRIPTABLE void stopAllStreams();
    Q_SCRIPTABLE QStringList activeStreams() const;
    Q_SCRIPTABLE QVariantMap streamInfo(const QString &streamId) const;

    QStringList remoteSources() const
    {
        return m_remoteCapabilities.sources;
    }

    void receivePacket(const NetworkPacket &np) override;
    void connected() override;
    QString dbusPath() const override;

Q_SIGNALS:
    Q_SCRIPTABLE void streamStarted(const QString &streamId);
    Q_SCRIPTABLE void streamStopped(const QString &streamId, const QString &error);
    Q_SCRIPTABLE void remoteSourcesChanged();

private:
    enum class State {
        Requested, // we asked the peer to send, waiting for its start packet
        AwaitingConfirmation, // the peer asked us to send, waiting for the local
                              // user
        Active,
    };

    struct Session {
        QString source;
        bool sending = false;
        State state = State::Requested;
        AudioStream::StreamFormat format;
        QPointer<AudioStream::OutgoingStream> outgoing;
        QPointer<AudioStream::IncomingStream> incoming;
        QPointer<QAudioSink> sink;
        QPointer<KNotification> notification;
        QPointer<QTimer> timeout;
    };

    void handleStreamPacket(const NetworkPacket &np);
    void handleRequestPacket(const NetworkPacket &np);

    void startOutgoing(const QString &streamId, const QString &source, const AudioStream::StreamFormat &format);
    void startIncoming(const QString &streamId, const AudioStream::Message &start, const QSharedPointer<QIODevice> &payload);
    void askConfirmation(const QString &streamId, const AudioStream::Message &request, const AudioStream::StreamFormat &format);
    // Tears down a session; notifies the peer when notifyPeer is set
    void endSession(const QString &streamId, const QString &error, bool notifyPeer);
    void showActiveNotification(const QString &streamId);
    void armTimeout(const QString &streamId, int ms);

    void sendStop(const QString &type, const QString &streamId, const QString &source, const QString &error);

    AudioStream::Capabilities m_localCapabilities;
    AudioStream::Capabilities m_remoteCapabilities;
    QMap<QString, Session> m_sessions;
};
