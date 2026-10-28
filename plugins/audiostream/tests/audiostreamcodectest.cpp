/**
 * SPDX-FileCopyrightText: 2026 KDE Connect Team <kdeconnect@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR
 * LicenseRef-KDE-Accepted-GPL
 */

#include "../audioframe.h"
#include "../audiostreamprotocol.h"
#include "../audiostreamsession.h"
#include "../framequeue.h"
#include "../jitterbuffer.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QTest>
#include <QtEndian>

using namespace AudioStream;

namespace
{
AudioFrame makeFrame(quint32 sequence, int size = 16, quint64 timestamp = 0)
{
    AudioFrame f;
    f.sequence = sequence;
    f.timestampUs = timestamp ? timestamp : 1000ull * sequence;
    f.data.resize(size);
    for (int i = 0; i < size; ++i) {
        f.data[i] = char((sequence * 31 + i) & 0xff);
    }
    return f;
}

QList<AudioFrame> drain(FrameDecoder &decoder)
{
    QList<AudioFrame> frames;
    while (auto f = decoder.next()) {
        frames.append(*f);
    }
    return frames;
}

// Pops one frame and returns its sequence, -1 for concealment, -2 for silence
qint64 popSequence(JitterBuffer &jb)
{
    const JitterBuffer::Output out = jb.pop();
    switch (out.kind) {
    case JitterBuffer::Output::Frame:
        return out.frame.sequence;
    case JitterBuffer::Output::Concealed:
        return -1;
    case JitterBuffer::Output::Silence:
        return -2;
    }
    return -3;
}
} // namespace

class AudioStreamCodecTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void frameWireFormat()
    {
        AudioFrame f;
        f.sequence = 0x01020304;
        f.timestampUs = 0x1122334455667788ull;
        f.last = true;
        f.data = QByteArrayLiteral("\xAA\xBB\xCC");
        const QByteArray wire = FrameCodec::encode(f);
        QCOMPARE(wire.size(), FrameCodec::HeaderSize + 3);
        QCOMPARE(wire.left(4), QByteArrayLiteral("KA\x01\x01"));
        QCOMPARE(wire.mid(4, 4), QByteArrayLiteral("\x01\x02\x03\x04"));
        QCOMPARE(wire.mid(8, 8), QByteArrayLiteral("\x11\x22\x33\x44\x55\x66\x77\x88"));
        QCOMPARE(wire.mid(16, 4), QByteArray("\x00\x00\x00\x03", 4));
        QCOMPARE(wire.mid(20), f.data);
    }

    void frameRoundTrip()
    {
        QByteArray wire;
        QList<AudioFrame> sent;
        for (quint32 i = 0; i < 50; ++i) {
            sent.append(makeFrame(0xFFFFFFF0u + i, int(i * 37 % 4000)));
            FrameCodec::encodeInto(wire, sent.last());
        }
        sent.last().last = true;
        wire.clear();
        for (const AudioFrame &f : std::as_const(sent)) {
            FrameCodec::encodeInto(wire, f);
        }

        FrameDecoder decoder;
        decoder.feed(wire);
        const QList<AudioFrame> received = drain(decoder);
        QCOMPARE(received.size(), sent.size());
        for (int i = 0; i < sent.size(); ++i) {
            QCOMPARE(received[i].sequence, sent[i].sequence);
            QCOMPARE(received[i].timestampUs, sent[i].timestampUs);
            QCOMPARE(received[i].data, sent[i].data);
            QCOMPARE(received[i].last, sent[i].last);
        }
        QCOMPARE(decoder.pendingBytes(), 0);
        QCOMPARE(decoder.discardedBytes(), 0);
    }

    void frameSplitAtEveryByte()
    {
        QByteArray wire;
        for (quint32 i = 0; i < 3; ++i) {
            FrameCodec::encodeInto(wire, makeFrame(i, 7));
        }
        for (int split = 0; split <= wire.size(); ++split) {
            FrameDecoder decoder;
            decoder.feed(wire.left(split));
            QList<AudioFrame> frames = drain(decoder);
            decoder.feed(wire.mid(split));
            frames += drain(decoder);
            QCOMPARE(frames.size(), 3);
            for (quint32 i = 0; i < 3; ++i) {
                QCOMPARE(frames[i].sequence, i);
                QCOMPARE(frames[i].data, makeFrame(i, 7).data);
            }
        }
    }

    void frameByteByByte()
    {
        QByteArray wire;
        for (quint32 i = 0; i < 20; ++i) {
            FrameCodec::encodeInto(wire, makeFrame(i, 100));
        }
        FrameDecoder decoder;
        QList<AudioFrame> frames;
        for (char c : std::as_const(wire)) {
            decoder.feed(QByteArray(1, c));
            frames += drain(decoder);
        }
        QCOMPARE(frames.size(), 20);
        QCOMPARE(frames.last().sequence, 19u);
    }

    void frameResyncAfterGarbage()
    {
        QByteArray wire = QByteArrayLiteral("garbage that is not a frame KA");
        const int garbage = wire.size();
        FrameCodec::encodeInto(wire, makeFrame(5));
        FrameCodec::encodeInto(wire, makeFrame(6));

        FrameDecoder decoder;
        decoder.feed(wire);
        const QList<AudioFrame> frames = drain(decoder);
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames[0].sequence, 5u);
        QCOMPARE(frames[1].sequence, 6u);
        QCOMPARE(decoder.discardedBytes(), garbage);
    }

    void frameRejectsOversizedAndUnknownVersion()
    {
        QByteArray bad = FrameCodec::encode(makeFrame(1));
        qToBigEndian<quint32>(FrameCodec::MaxDataSize + 1, reinterpret_cast<uchar *>(bad.data()) + 16);
        QByteArray badVersion = FrameCodec::encode(makeFrame(2));
        badVersion[2] = 2;

        FrameDecoder decoder;
        decoder.feed(bad + badVersion + FrameCodec::encode(makeFrame(3)));
        const QList<AudioFrame> frames = drain(decoder);
        // The headers of both bad frames are skipped; nothing in their bodies looks
        // like a frame
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames[0].sequence, 3u);
        QVERIFY(decoder.discardedBytes() > 0);
    }

    void frameMaxSizeAccepted()
    {
        const AudioFrame big = makeFrame(9, int(FrameCodec::MaxDataSize));
        FrameDecoder decoder;
        decoder.feed(FrameCodec::encode(big));
        const QList<AudioFrame> frames = drain(decoder);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames[0].data, big.data);
    }

    void jitterPrefillAndInOrder()
    {
        JitterBuffer jb(3, 10);
        QCOMPARE(popSequence(jb), -2);
        jb.push(makeFrame(0));
        jb.push(makeFrame(1));
        QCOMPARE(popSequence(jb), -2);
        QCOMPARE(jb.state(), JitterBuffer::State::Buffering);
        jb.push(makeFrame(2));
        QCOMPARE(popSequence(jb), 0);
        QCOMPARE(jb.state(), JitterBuffer::State::Playing);
        jb.push(makeFrame(3));
        QCOMPARE(popSequence(jb), 1);
        QCOMPARE(popSequence(jb), 2);
        QCOMPARE(popSequence(jb), 3);
        QCOMPARE(jb.stats().played, 4u);
        QCOMPARE(jb.stats().lost, 0u);
    }

    void jitterReorders()
    {
        JitterBuffer jb(4, 10);
        for (quint32 s : {2u, 0u, 3u, 1u}) {
            jb.push(makeFrame(s));
        }
        for (qint64 s = 0; s < 4; ++s) {
            QCOMPARE(popSequence(jb), s);
        }
        QCOMPARE(jb.stats().lost, 0u);
    }

    void jitterConcealsLoss()
    {
        JitterBuffer jb(2, 10);
        jb.push(makeFrame(0));
        jb.push(makeFrame(1));
        jb.push(makeFrame(3));
        jb.push(makeFrame(4));
        QCOMPARE(popSequence(jb), 0);
        QCOMPARE(popSequence(jb), 1);
        QCOMPARE(popSequence(jb), -1); // 2 is missing, 3 is there
        QCOMPARE(popSequence(jb), 3);
        QCOMPARE(popSequence(jb), 4);
        QCOMPARE(jb.stats().lost, 1u);
    }

    void jitterLateAndDuplicate()
    {
        JitterBuffer jb(1, 10);
        jb.push(makeFrame(0));
        jb.push(makeFrame(1));
        jb.push(makeFrame(1));
        QCOMPARE(jb.stats().duplicates, 1u);
        QCOMPARE(popSequence(jb), 0);
        QCOMPARE(popSequence(jb), 1);
        jb.push(makeFrame(0)); // already played
        QCOMPARE(jb.stats().late, 1u);
        QCOMPARE(jb.depth(), 0);
    }

    void jitterUnderrunRebuffers()
    {
        JitterBuffer jb(2, 10);
        jb.push(makeFrame(0));
        jb.push(makeFrame(1));
        QCOMPARE(popSequence(jb), 0);
        QCOMPARE(popSequence(jb), 1);
        QCOMPARE(popSequence(jb), -2);
        QCOMPARE(jb.stats().underruns, 1u);
        QCOMPARE(jb.state(), JitterBuffer::State::Buffering);
        // Frame 2 never comes; playing resumes at 3 once the target is reached
        // again
        jb.push(makeFrame(3));
        QCOMPARE(popSequence(jb), -2);
        jb.push(makeFrame(4));
        QCOMPARE(popSequence(jb), 3);
        QCOMPARE(jb.stats().lost, 1u);
        QCOMPARE(popSequence(jb), 4);
    }

    void jitterOverflowTrimsToTarget()
    {
        JitterBuffer jb(3, 6);
        for (quint32 s = 0; s < 7; ++s) {
            jb.push(makeFrame(s));
        }
        // 7 > 6: trimmed back to the 3 newest
        QCOMPARE(jb.depth(), 3);
        QCOMPARE(jb.stats().overflowDropped, 4u);
        QCOMPARE(popSequence(jb), 4);

        // While playing, too: 5, 6 left, 7..11 pushes it to 7 > 6
        for (quint32 s = 7; s < 12; ++s) {
            jb.push(makeFrame(s));
        }
        QCOMPARE(jb.depth(), 3);
        QCOMPARE(popSequence(jb), 9);
        QCOMPARE(popSequence(jb), 10);
        QCOMPARE(popSequence(jb), 11);
        QCOMPARE(jb.stats().lost, 0u);
    }

    void jitterSequenceWrap()
    {
        JitterBuffer jb(2, 10);
        jb.push(makeFrame(0xFFFFFFFFu));
        jb.push(makeFrame(0xFFFFFFFEu));
        jb.push(makeFrame(0));
        jb.push(makeFrame(1));
        QCOMPARE(popSequence(jb), 0xFFFFFFFEu);
        QCOMPARE(popSequence(jb), 0xFFFFFFFFu);
        QCOMPARE(popSequence(jb), 0);
        QCOMPARE(popSequence(jb), 1);
        QCOMPARE(jb.stats().lost, 0u);
    }

    void jitterTargets()
    {
        QCOMPARE(JitterBuffer::targetFramesFor(80, 20), 4);
        QCOMPARE(JitterBuffer::targetFramesFor(90, 20), 5);
        QCOMPARE(JitterBuffer::targetFramesFor(1, 20), 1);
        QCOMPARE(JitterBuffer::maxFramesFor(80, 20), 14);
    }

    void playbackDeviceFillsSilence()
    {
        StreamFormat format;
        format.sampleRate = 1000;
        format.channels = 1;
        format.frameDurationMs = 10; // 10 samples, 20 bytes
        PlaybackDevice device(format, 20); // target 2 frames
        QVERIFY(device.open(QIODevice::ReadOnly | QIODevice::Unbuffered));

        // Buffering: all silence
        QByteArray out = device.read(50);
        QCOMPARE(out, QByteArray(50, '\0'));

        AudioFrame a = makeFrame(0, 20);
        AudioFrame b = makeFrame(2, 20);
        AudioFrame c = makeFrame(3, 20);
        device.push(a);
        device.push(b);
        device.push(c);
        out = device.read(80);
        QCOMPARE(out.size(), 80);
        QCOMPARE(out.mid(0, 20), a.data);
        QCOMPARE(out.mid(20, 20), QByteArray(20, '\0')); // concealed frame 1
        QCOMPARE(out.mid(40, 20), b.data);
        QCOMPARE(out.mid(60, 20), c.data);
        QCOMPARE(device.stats().lost, 1u);

        // Odd sized read is cut to whole samples
        QCOMPARE(device.read(5).size(), 4);
    }

    void frameQueueDropsOldestWholeFrames()
    {
        const int frameWire = FrameCodec::HeaderSize + 10;
        FrameQueue queue(frameWire * 3);
        queue.enqueue(1, QByteArray(10, 'x')); // not open yet: ignored
        QCOMPARE(queue.framesQueued(), 0u);
        QVERIFY(queue.open(QIODevice::ReadOnly));
        for (int i = 0; i < 5; ++i) {
            queue.enqueue(quint64(i), QByteArray(10, char('a' + i)));
        }
        QCOMPARE(queue.framesQueued(), 5u);
        QCOMPARE(queue.framesDropped(), 2u);
        QCOMPARE(queue.bytesAvailable(), qint64(frameWire * 3));

        // Read half a frame: that frame must survive the next drops
        FrameDecoder decoder;
        decoder.feed(queue.read(frameWire / 2));
        for (int i = 5; i < 9; ++i) {
            queue.enqueue(quint64(i), QByteArray(10, char('a' + i)));
        }
        decoder.feed(queue.readAll());
        const QList<AudioFrame> frames = drain(decoder);
        QVERIFY(frames.size() >= 2);
        QCOMPARE(frames.first().sequence, 2u);
        QCOMPARE(frames.first().data, QByteArray(10, 'c'));
        QCOMPARE(frames.last().sequence, 8u);
        QCOMPARE(decoder.discardedBytes(), 0);
        // Sequence numbers of dropped frames are skipped, not reused
        for (int i = 1; i < frames.size(); ++i) {
            QVERIFY(frames[i].sequence > frames[i - 1].sequence);
        }
    }

    void frameQueueFinishClosesWhenDrained()
    {
        FrameQueue queue(1 << 20);
        QVERIFY(queue.open(QIODevice::ReadOnly));
        queue.enqueue(1, QByteArray(4, 'a'));
        QSignalSpy closing(&queue, &QIODevice::aboutToClose);
        queue.finish();
        queue.enqueue(2, QByteArray(4, 'b')); // ignored after finish
        FrameDecoder decoder;
        decoder.feed(queue.readAll());
        const QList<AudioFrame> frames = drain(decoder);
        QCOMPARE(frames.size(), 2);
        QVERIFY(!frames[0].last);
        QVERIFY(frames[1].last);
        QCOMPARE(frames[1].sequence, 1u);
        QVERIFY(closing.wait(1000));
        QVERIFY(!queue.isOpen());
    }

    void parseStartRequest()
    {
        NetworkPacket np = makeStartRequest(QStringLiteral("abc-1"), Source::Media, StreamFormat{});
        QCOMPARE(np.type(), PACKET_TYPE_AUDIOSTREAM_REQUEST);
        // Round trip through JSON like on the wire
        NetworkPacket parsed;
        QVERIFY(NetworkPacket::unserialize(np.serialize(), &parsed));
        Message m;
        QCOMPARE(parseMessage(parsed, &m), QString());
        QCOMPARE(m.action, Action::Start);
        QCOMPARE(m.streamId, QStringLiteral("abc-1"));
        QCOMPARE(m.source, Source::Media);
        QCOMPARE(m.format.codec, Codec::PcmS16le);
        QCOMPARE(m.format.sampleRate, 48000);
        QCOMPARE(m.format.channels, 2);
        QCOMPARE(m.format.frameDurationMs, DefaultFrameDurationMs);
    }

    void parseStartPacket()
    {
        StreamFormat f;
        f.channels = 1;
        f.frameDurationMs = 10;
        NetworkPacket np = makeStartPacket(QStringLiteral("s1"), Source::Call, f, QStringLiteral("full"));
        np.setPayload(QSharedPointer<QIODevice>(new QBuffer), -1);
        QVERIFY(np.serialize().contains("\"payloadSize\":-1"));
        NetworkPacket parsed;
        QVERIFY(NetworkPacket::unserialize(np.serialize(), &parsed));
        QCOMPARE(parsed.payloadSize(), -1);
        Message m;
        QCOMPARE(parseMessage(parsed, &m), QString());
        QCOMPARE(m.format, f);
        QCOMPARE(m.callAudio, QStringLiteral("full"));
        QCOMPARE(f.frameBytes(), 960);
    }

    void parseRejectsInvalid_data()
    {
        QTest::addColumn<QVariantMap>("body");
        QTest::addColumn<QString>("error");
        const QVariantMap good{
            {QStringLiteral("action"), QStringLiteral("start")},
            {QStringLiteral("streamId"), QStringLiteral("x")},
            {QStringLiteral("source"), QStringLiteral("media")},
            {QStringLiteral("codec"), QStringLiteral("pcm_s16le")},
            {QStringLiteral("sampleRate"), 48000},
            {QStringLiteral("channels"), 2},
        };
        auto with = [&](const QString &key, const QVariant &value) {
            QVariantMap m = good;
            if (value.isValid()) {
                m[key] = value;
            } else {
                m.remove(key);
            }
            return m;
        };
        QTest::newRow("bad action") << with(QStringLiteral("action"), QStringLiteral("play")) << Error::InvalidRequest;
        QTest::newRow("no action") << with(QStringLiteral("action"), QVariant()) << Error::InvalidRequest;
        QTest::newRow("no streamId") << with(QStringLiteral("streamId"), QVariant()) << Error::InvalidRequest;
        QTest::newRow("bad streamId") << with(QStringLiteral("streamId"), QStringLiteral("a b")) << Error::InvalidRequest;
        QTest::newRow("long streamId") << with(QStringLiteral("streamId"), QString(65, QLatin1Char('a'))) << Error::InvalidRequest;
        QTest::newRow("bad source") << with(QStringLiteral("source"), QStringLiteral("radio")) << Error::InvalidRequest;
        QTest::newRow("no codec") << with(QStringLiteral("codec"), QVariant()) << Error::InvalidRequest;
        QTest::newRow("opus") << with(QStringLiteral("codec"), QStringLiteral("opus")) << Error::UnsupportedFormat;
        QTest::newRow("rate low") << with(QStringLiteral("sampleRate"), 100) << Error::InvalidRequest;
        QTest::newRow("rate string") << with(QStringLiteral("sampleRate"), QStringLiteral("48000")) << Error::InvalidRequest;
        QTest::newRow("rate fraction") << with(QStringLiteral("sampleRate"), 44100.5) << Error::InvalidRequest;
        QTest::newRow("channels 3") << with(QStringLiteral("channels"), 3) << Error::InvalidRequest;
        QTest::newRow("no channels") << with(QStringLiteral("channels"), QVariant()) << Error::InvalidRequest;
        QTest::newRow("frame duration") << with(QStringLiteral("frameDurationMs"), 1000) << Error::InvalidRequest;
        QTest::newRow("good") << good << QString();
    }

    void parseRejectsInvalid()
    {
        QFETCH(QVariantMap, body);
        QFETCH(QString, error);
        Message m;
        QCOMPARE(parseMessage(NetworkPacket(PACKET_TYPE_AUDIOSTREAM, body), &m), error);
    }

    void parseStopAndCapabilities()
    {
        Message m;
        NetworkPacket stop = makeStopPacket(PACKET_TYPE_AUDIOSTREAM, QStringLiteral("s"), Source::Microphone, Error::PermissionDenied);
        QCOMPARE(parseMessage(stop, &m), QString());
        QCOMPARE(m.action, Action::Stop);
        QCOMPARE(m.error, Error::PermissionDenied);
        // stop needs nothing but the stream id
        QCOMPARE(parseMessage(NetworkPacket(PACKET_TYPE_AUDIOSTREAM_REQUEST,
                                            {{QStringLiteral("action"), QStringLiteral("stop")}, {QStringLiteral("streamId"), QStringLiteral("s")}}),
                              &m),
                 QString());
        QVERIFY(m.error.isEmpty());

        QCOMPARE(parseMessage(makeCapabilitiesRequest(), &m), QString());
        QCOMPARE(m.action, Action::Capabilities);

        Capabilities caps;
        caps.sources = {Source::Microphone, Source::Call};
        caps.canPlay = false;
        caps.maxStreams = 2;
        NetworkPacket wire;
        QVERIFY(NetworkPacket::unserialize(makeCapabilitiesPacket(caps).serialize(), &wire));
        const Capabilities parsed = parseCapabilities(wire);
        QCOMPARE(parsed.sources, caps.sources);
        QCOMPARE(parsed.codecs, QStringList{Codec::PcmS16le});
        QCOMPARE(parsed.sampleRates, QList<int>{48000});
        QCOMPARE(parsed.canPlay, false);
        QCOMPARE(parsed.maxStreams, 2);

        // Unknown sources from a newer peer are ignored
        const Capabilities other = parseCapabilities(NetworkPacket(
            PACKET_TYPE_AUDIOSTREAM,
            {{QStringLiteral("action"), QStringLiteral("capabilities")}, {QStringLiteral("sources"), QStringList{QStringLiteral("hologram"), Source::Media}}}));
        QCOMPARE(other.sources, QStringList{Source::Media});
        QCOMPARE(other.maxStreams, MaxStreams);
        QCOMPARE(other.canPlay, true);
    }

    void negotiation()
    {
        Capabilities local;
        local.sources = {Source::Media, Source::Microphone};
        Message request;
        request.action = Action::Start;
        request.streamId = QStringLiteral("s");
        request.source = Source::Media;

        // Stereo source, mono requested: downmix
        request.format.channels = 1;
        Decision d = decideStartRequest(request, local, 0, false, 2);
        QVERIFY(d.error.isEmpty());
        QCOMPARE(d.format.channels, 1);

        // Mono source, stereo requested: stays mono
        request.format.channels = 2;
        d = decideStartRequest(request, local, 0, false, 1);
        QCOMPARE(d.format.channels, 1);

        // Unsupported rate: fall back to the default format
        request.format.sampleRate = 44100;
        request.format.channels = 1;
        d = decideStartRequest(request, local, 0, false, 2);
        QVERIFY(d.error.isEmpty());
        QCOMPARE(d.format.sampleRate, 48000);
        QCOMPARE(d.format.channels, 2);

        request.source = Source::System;
        QCOMPARE(decideStartRequest(request, local, 0, false, 2).error, Error::UnsupportedSource);
        request.source = Source::Media;
        QCOMPARE(decideStartRequest(request, local, MaxStreams, false, 2).error, Error::Busy);
        QCOMPARE(decideStartRequest(request, local, 0, true, 2).error, Error::Busy);
        request.format.codec = QStringLiteral("opus");
        QCOMPARE(decideStartRequest(request, local, 0, false, 2).error, Error::UnsupportedFormat);

        Message start;
        start.format = StreamFormat{};
        QCOMPARE(checkIncomingStart(start, local, 0, false), QString());
        QCOMPARE(checkIncomingStart(start, local, MaxStreams, false), Error::Busy);
        local.canPlay = false;
        QCOMPARE(checkIncomingStart(start, local, 0, false), Error::PlaybackFailed);
    }

    void streamIds()
    {
        const QString a = newStreamId();
        QVERIFY(isValidStreamId(a));
        QVERIFY(a != newStreamId());
        QVERIFY(!isValidStreamId(QString()));
        QVERIFY(isValidStreamId(QStringLiteral("call-1:a.b_c")));
    }
};

QTEST_GUILESS_MAIN(AudioStreamCodecTest)

#include "audiostreamcodectest.moc"
