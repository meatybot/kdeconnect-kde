/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

// End to end: an audio stream sent as an endless payload by the real LAN upload
// job, over a local TLS socket, into the receiving pipeline (decoder + jitter
// buffer + playback device).

#include <QSignalSpy>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTest>

#include <backends/lan/compositeuploadjob.h>
#include <backends/lan/lanlinkprovider.h>
#include <backends/lan/uploadjob.h>
#include <kdeconnectconfig.h>

#include "plugins/audiostream/audiostreamprotocol.h"
#include "plugins/audiostream/audiostreamsession.h"
#include "testdaemon.h"
#include "testdevice.h"

using namespace AudioStream;

namespace
{
// Stands in for the sound server: frames are produced when the test says so
class FakeCapture : public AudioCapture
{
    Q_OBJECT
public:
    explicit FakeCapture(const StreamFormat &format)
        : m_format(format)
    {
    }
    void start() override
    {
        running = true;
    }
    void stop() override
    {
        running = false;
    }
    StreamFormat format() const override
    {
        return m_format;
    }
    QByteArray produce(quint32 n)
    {
        QByteArray pcm(m_format.frameBytes(), '\0');
        for (int i = 0; i < pcm.size(); ++i) {
            pcm[i] = char((n * 7 + i) & 0xff);
        }
        Q_EMIT frameCaptured(monotonicUs(), pcm);
        return pcm;
    }
    bool running = false;

private:
    StreamFormat m_format;
};
} // namespace

class AudioStreamTest : public QObject
{
    Q_OBJECT
public:
    AudioStreamTest()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_daemon = new TestDaemon;
    }

private:
    struct Pipe {
        TestDevice *device = nullptr;
        CompositeUploadJob *job = nullptr;
        OutgoingStream *outgoing = nullptr;
        FakeCapture *capture = nullptr;
        IncomingStream *incoming = nullptr;
        QList<AudioFrame> received;
    };

    // Sets up sender and receiver and waits until the receiver is connected
    void connectPipe(Pipe &p, const StreamFormat &format)
    {
        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);
        p.device = new TestDevice(this, deviceInfo.id);

        p.capture = new FakeCapture(format);
        p.outgoing = new OutgoingStream(QStringLiteral("e2e"), Source::Media, p.capture, this);
        NetworkPacket np = makeStartPacket(p.outgoing->streamId(), p.outgoing->source(), format);
        np.setPayload(p.outgoing->payload(), -1);

        p.job = new CompositeUploadJob(p.device, false);
        p.job->addSubjob(new UploadJob(np));
        QSignalSpy connectedSpy(p.outgoing, &OutgoingStream::connected);
        QObject::connect(p.outgoing, &OutgoingStream::connected, p.outgoing, &OutgoingStream::start);
        p.job->start();

        QTRY_COMPARE_WITH_TIMEOUT(p.device->getSentPackets(), 1, 5000);
        NetworkPacket *announced = p.device->getLastPacket();
        QCOMPARE(announced->payloadSize(), -1);
        Message start;
        QCOMPARE(parseMessage(*announced, &start), QString());
        QCOMPARE(start.format, format);
        const quint16 port = announced->payloadTransferInfo().value(QStringLiteral("port")).toInt();
        QVERIFY(port);

        // What LanDeviceLink::dataReceived() does for a packet with a payload
        QSharedPointer<QSslSocket> socket(new QSslSocket);
        LanLinkProvider::configureSslSocket(socket.data(), deviceInfo.id, true);
        connect(socket.data(), &QAbstractSocket::disconnected, socket.data(), &QAbstractSocket::readChannelFinished);
        socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, QIODevice::ReadWrite);

        p.incoming = new IncomingStream(start.streamId, start.source, start.format, socket, 40, this);
        connect(p.incoming, &IncomingStream::frameDecoded, p.incoming, [&p](const AudioFrame &f) {
            p.received.append(f);
        });
        p.incoming->start();

        QVERIFY(connectedSpy.count() || connectedSpy.wait(5000));
        QVERIFY(p.capture->running);
    }

private Q_SLOTS:
    void streamSurvivesIdleAndEndsCleanly()
    {
        StreamFormat format;
        format.channels = 2;
        Pipe p;
        connectPipe(p, format);
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy jobResult(p.job, &KJob::result);
        QSignalSpy incomingFinished(p.incoming, &IncomingStream::finished);

        QList<QByteArray> sent;
        for (quint32 i = 0; i < 10; ++i) {
            sent.append(p.capture->produce(i));
        }
        QTRY_COMPARE_WITH_TIMEOUT(p.received.size(), 10, 5000);

        // What the audio output would play: the frames, in order
        PlaybackDevice *playback = p.incoming->playbackDevice();
        QByteArray played = playback->read(qint64(format.frameBytes()) * 10);
        QCOMPARE(played.size(), format.frameBytes() * 10);
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(played.mid(i * format.frameBytes(), format.frameBytes()), sent[i]);
        }
        QCOMPARE(playback->stats().lost, 0u);

        // A live stream with nothing to send for a while must not be ended by the
        // upload job
        QTest::qWait(1500);
        QCOMPARE(jobResult.count(), 0);
        QCOMPARE(incomingFinished.count(), 0);

        for (quint32 i = 10; i < 20; ++i) {
            sent.append(p.capture->produce(i));
        }
        QTRY_COMPARE_WITH_TIMEOUT(p.received.size(), 20, 5000);
        for (int i = 0; i < sent.size(); ++i) {
            QCOMPARE(p.received[i].sequence, quint32(i));
            QCOMPARE(p.received[i].data, sent[i]);
        }
        QVERIFY(p.incoming->jitterMs() >= 0);
        played = playback->read(qint64(format.frameBytes()) * 10);
        QCOMPARE(played.mid(0, format.frameBytes()), sent[10]);
        QCOMPARE(playback->stats().lost, 0u);

        // Sender stops: the last frame ends the stream on the receiver, the upload
        // job finishes
        QSignalSpy outgoingFinished(p.outgoing, &OutgoingStream::finished);
        p.outgoing->finish();
        QVERIFY(!p.capture->running);
        QVERIFY(incomingFinished.count() || incomingFinished.wait(5000));
        QCOMPARE(incomingFinished.first().first().toString(), QString());
        QVERIFY(outgoingFinished.count() || outgoingFinished.wait(5000));
        QVERIFY(jobResult.count() || jobResult.wait(5000));
        QCOMPARE(p.job->error(), 0);
        delete p.incoming;
        delete p.outgoing;
    }

    void receiverClosingStopsSender()
    {
        StreamFormat format;
        format.channels = 1;
        format.frameDurationMs = 10;
        Pipe p;
        connectPipe(p, format);
        if (QTest::currentTestFailed()) {
            return;
        }
        p.capture->produce(0);
        QTRY_COMPARE_WITH_TIMEOUT(p.received.size(), 1, 5000);

        QSignalSpy outgoingFinished(p.outgoing, &OutgoingStream::finished);
        p.incoming->close();
        // The upload job notices the disconnection and closes the payload source
        QVERIFY(outgoingFinished.wait(5000));
        QCOMPARE(outgoingFinished.first().first().toString(), QString());
        QVERIFY(!p.capture->running);
        // Capturing more after that is harmless
        p.capture->produce(1);
        delete p.incoming;
        delete p.outgoing;
    }

    void concurrentStreams()
    {
        StreamFormat stereo;
        StreamFormat mono;
        mono.channels = 1;
        Pipe a;
        Pipe b;
        connectPipe(a, stereo);
        if (QTest::currentTestFailed()) {
            return;
        }
        connectPipe(b, mono);
        if (QTest::currentTestFailed()) {
            return;
        }
        for (quint32 i = 0; i < 5; ++i) {
            a.capture->produce(i);
            b.capture->produce(100 + i);
        }
        QTRY_COMPARE_WITH_TIMEOUT(a.received.size(), 5, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(b.received.size(), 5, 5000);
        QCOMPARE(a.received.first().data.size(), stereo.frameBytes());
        QCOMPARE(b.received.first().data.size(), mono.frameBytes());
        a.outgoing->finish();
        b.outgoing->finish();
        QSignalSpy aDone(a.incoming, &IncomingStream::finished);
        QSignalSpy bDone(b.incoming, &IncomingStream::finished);
        QVERIFY(aDone.count() || aDone.wait(5000));
        QVERIFY(bDone.count() || bDone.wait(5000));
        delete a.incoming;
        delete b.incoming;
        delete a.outgoing;
        delete b.outgoing;
    }

private:
    TestDaemon *m_daemon;
};

QTEST_MAIN(AudioStreamTest)

#include "audiostreamtest.moc"
