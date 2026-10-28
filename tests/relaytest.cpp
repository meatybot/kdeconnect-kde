/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTest>
#include <QUuid>

#include <memory>

#include "core/backends/relay/relayconnection.h"
#include "core/backends/relay/relaydevicelink.h"
#include "core/backends/relay/relaylinkprovider.h"
#include "core/backends/relay/relayprotocol.h"
#include "core/backends/relay/relaytunnelsocket.h"
#include "core/connecttargets.h"
#include "core/kdeconnectconfig.h"
#include "core/networkpacket.h"
#include "core/sslhelper.h"
#include "relayserver/relayserver.h"

using RelayProtocol::Target;

namespace
{
struct Identity {
    QString deviceId;
    QSslKey key;
    QSslCertificate certificate;

    static Identity generate(bool rsa = false)
    {
        Identity id;
        id.deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces).replace(QLatin1Char('-'), QLatin1Char('_'));
        id.key = rsa ? SslHelper::generateRsaPrivateKey() : SslHelper::generateEcPrivateKey();
        id.certificate = SslHelper::generateSelfSignedCertificate(id.key, id.deviceId);
        return id;
    }
    Target keyTarget() const
    {
        return {Target::Key, RelayProtocol::relayKey(certificate)};
    }
    Target fingerprintTarget() const
    {
        return {Target::Fingerprint, RelayProtocol::fingerprint(certificate)};
    }
};

struct Client {
    Identity identity;
    std::unique_ptr<RelayConnection> connection;
};

QByteArray readExactly(QIODevice *device, qint64 size, int timeoutMs = 10000)
{
    QByteArray result;
    QElapsedTimer timer;
    timer.start();
    while (result.size() < size && timer.elapsed() < timeoutMs) {
        result += device->read(size - result.size());
        if (result.size() < size) {
            QTest::qWait(5);
        }
    }
    return result;
}

QByteArray readLine(QIODevice *device, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!device->canReadLine() && timer.elapsed() < timeoutMs) {
        QTest::qWait(5);
    }
    return device->readLine();
}

QByteArray pattern(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) {
        data[i] = char((i * 31 + 7) & 0xff);
    }
    return data;
}
}

class RelayTest : public QObject
{
    Q_OBJECT

public:
    RelayTest()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

private:
    std::unique_ptr<RelayServer> startServer(const RelayServerLimits &limits = {})
    {
        auto server = std::make_unique<RelayServer>(m_serverIdentity.certificate, m_serverIdentity.key, limits);
        if (!server->listen(QHostAddress::LocalHost, 0)) {
            return nullptr;
        }
        return server;
    }

    Client registerClient(RelayServer *server, const Identity &identity = Identity::generate())
    {
        Client client{identity, std::make_unique<RelayConnection>(identity.certificate, identity.key)};
        client.connection->setServer(QStringLiteral("127.0.0.1"), server->serverPort());
        client.connection->start();
        // The caller checks the status, test macros can't be used in a function returning a value
        (void)QTest::qWaitFor(
            [&client]() {
                return client.connection->status() == RelayConnection::Status::Registered;
            },
            10000);
        return client;
    }

    // Requests a link tunnel from a to b and returns the token both sides got
    QString linkTunnel(Client &a, Client &b, const Target &target)
    {
        QSignalSpy ready(a.connection.get(), &RelayConnection::tunnelReady);
        QSignalSpy incoming(b.connection.get(), &RelayConnection::incoming);
        const QString requestId = a.connection->requestTunnel(target, QStringLiteral("link"));
        if (requestId.isEmpty() || !ready.wait(5000) || (incoming.isEmpty() && !incoming.wait(5000))) {
            return QString();
        }
        if (ready.at(0).at(0).toString() != requestId || ready.at(0).at(1).toString() != incoming.at(0).at(0).toString()
            || ready.at(0).at(3).toString() != RelayProtocol::fingerprint(b.identity.certificate)
            || incoming.at(0).at(1).toString() != RelayProtocol::fingerprint(a.identity.certificate)) {
            return QString();
        }
        return ready.at(0).at(1).toString();
    }

    RelayTunnelSocket *openTunnel(RelayServer *server, const Identity &identity, const QString &token, RelayTunnelSocket::Role role, const Target &pin)
    {
        auto *socket = new RelayTunnelSocket(this);
        socket->open(QStringLiteral("127.0.0.1"), server->serverPort(), token, role, identity.certificate, identity.key, pin);
        return socket;
    }

    QString requestError(Client &client, const Target &target, const QString &purpose = QStringLiteral("link"))
    {
        QSignalSpy failed(client.connection.get(), &RelayConnection::requestFailed);
        QSignalSpy ready(client.connection.get(), &RelayConnection::tunnelReady);
        const QString requestId = client.connection->requestTunnel(target, purpose);
        if (requestId.isEmpty()) {
            return QStringLiteral("not sent");
        }
        QElapsedTimer timer;
        timer.start();
        while (failed.isEmpty() && ready.isEmpty() && timer.elapsed() < 5000) {
            QTest::qWait(5);
        }
        if (!ready.isEmpty()) {
            return QStringLiteral("ok");
        }
        if (failed.isEmpty() || failed.at(0).at(0).toString() != requestId) {
            return QStringLiteral("no reply");
        }
        return failed.at(0).at(1).toString();
    }

    Identity m_serverIdentity;

private Q_SLOTS:
    void initTestCase()
    {
        m_serverIdentity = Identity::generate();
        QVERIFY(!m_serverIdentity.certificate.isNull());
    }

    void testSharedTestVector()
    {
        QFile file(QFINDTESTDATA("data/relay-test-vector.pem"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QSslCertificate certificate(file.readAll(), QSsl::Pem);
        QVERIFY(!certificate.isNull());
        // Same expected values as the Android unit test, see docs/specs/relay.md
        QCOMPARE(RelayProtocol::fingerprint(certificate), QStringLiteral("fc1fb3d3d33b2342e45c74b1a613dcdfe5e1f029d668249f504952a9a8041e31"));
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(certificate.publicKey().toDer(), QCryptographicHash::Sha256).toHex()),
                 QStringLiteral("a2d2e306d0372fb596fd6f647a05d6b79054b5ee052ea58171efc3144d54fe8c"));
        QCOMPARE(RelayProtocol::relayKey(certificate), QStringLiteral("ULJO-GBWQ-G4X3-LFX5-N5SH"));
    }

    void testKeyDeterminismAndUniqueness()
    {
        const Identity a = Identity::generate();
        const Identity b = Identity::generate();
        const Identity rsa = Identity::generate(true);
        QCOMPARE(RelayProtocol::relayKey(a.certificate), RelayProtocol::relayKey(QSslCertificate(a.certificate.toPem())));
        QVERIFY(RelayProtocol::relayKey(a.certificate) != RelayProtocol::relayKey(b.certificate));
        QVERIFY(RelayProtocol::relayKey(a.certificate) != RelayProtocol::relayKey(rsa.certificate));
        QCOMPARE(RelayProtocol::relayKey(rsa.certificate).size(), 24);

        // The key only depends on the public key, the fingerprint on the whole certificate
        const QSslCertificate reissued = SslHelper::generateSelfSignedCertificate(a.key, QStringLiteral("another_common_name_0000000000000"));
        QCOMPARE(RelayProtocol::relayKey(reissued), RelayProtocol::relayKey(a.certificate));
        QVERIFY(RelayProtocol::fingerprint(reissued) != RelayProtocol::fingerprint(a.certificate));
        QVERIFY(RelayProtocol::relayKey(QSslCertificate()).isEmpty());
    }

    void testParseTarget()
    {
        QCOMPARE(RelayProtocol::parseTarget(QStringLiteral(" uljo gbwq-g4x3=lfx5n5sh ")), (Target{Target::Key, QStringLiteral("ULJO-GBWQ-G4X3-LFX5-N5SH")}));
        const QString colons = QStringLiteral("FC:1F:B3:D3:D3:3B:23:42:E4:5C:74:B1:A6:13:DC:DF:E5:E1:F0:29:D6:68:24:9F:50:49:52:A9:A8:04:1E:31");
        QCOMPARE(RelayProtocol::parseTarget(colons),
                 (Target{Target::Fingerprint, QStringLiteral("fc1fb3d3d33b2342e45c74b1a613dcdfe5e1f029d668249f504952a9a8041e31")}));
        QVERIFY(!RelayProtocol::parseTarget(QStringLiteral("ULJO-GBWQ-G4X3-LFX5-N5S1")).isValid()); // 1 is not base32
        QVERIFY(!RelayProtocol::parseTarget(QStringLiteral("ULJO-GBWQ-G4X3-LFX5")).isValid());
        QVERIFY(!RelayProtocol::parseTarget(QString(64, QLatin1Char('g'))).isValid());
        QVERIFY(!RelayProtocol::parseTarget(QString()).isValid());
        QVERIFY(!RelayProtocol::parseTarget(QStringLiteral("ULJO-GBWQ-G4X3-LFX5-N5S\u00c4")).isValid());

        const Identity a = Identity::generate();
        const Identity b = Identity::generate();
        QVERIFY(RelayProtocol::matches(a.keyTarget(), a.certificate));
        QVERIFY(RelayProtocol::matches(a.fingerprintTarget(), a.certificate));
        QVERIFY(!RelayProtocol::matches(a.keyTarget(), b.certificate));
        QVERIFY(!RelayProtocol::matches(a.fingerprintTarget(), b.certificate));
        QVERIFY(!RelayProtocol::matches(Target(), a.certificate));
        QVERIFY(!RelayProtocol::matches(a.keyTarget(), QSslCertificate()));
    }

    void testMessageCodec()
    {
        const QJsonObject message{{QStringLiteral("type"), QStringLiteral("connect")}, {QStringLiteral("target"), QStringLiteral("x")}};
        const QByteArray line = RelayProtocol::encode(message);
        QVERIFY(line.endsWith('\n'));
        QCOMPARE(line.count('\n'), 1);
        QJsonObject decoded;
        QVERIFY(RelayProtocol::decode(line, &decoded));
        QCOMPARE(decoded, message);

        QVERIFY(!RelayProtocol::decode("{\"target\":\"x\"}\n", &decoded)); // No type
        QVERIFY(!RelayProtocol::decode("{\"type\":1}\n", &decoded));
        QVERIFY(!RelayProtocol::decode("[1,2]\n", &decoded));
        QVERIFY(!RelayProtocol::decode("{\"type\":\"x\"\n", &decoded));
        QByteArray big = "{\"type\":\"x\",\"pad\":\"" + QByteArray(RelayProtocol::MaxLineSize, 'a') + "\"}\n";
        QVERIFY(!RelayProtocol::decode(big, &decoded));
    }

    void testParseAddress()
    {
        QString host;
        quint16 port = 0;
        QVERIFY(RelayProtocol::parseAddress(QStringLiteral("relay.example.org"), &host, &port));
        QCOMPARE(host, QStringLiteral("relay.example.org"));
        QCOMPARE(port, RelayProtocol::DefaultPort);
        QVERIFY(RelayProtocol::parseAddress(QStringLiteral(" 10.0.0.1:4000 "), &host, &port));
        QCOMPARE(host, QStringLiteral("10.0.0.1"));
        QCOMPARE(port, quint16(4000));
        QVERIFY(RelayProtocol::parseAddress(QStringLiteral("[::1]:99"), &host, &port));
        QCOMPARE(host, QStringLiteral("::1"));
        QCOMPARE(port, quint16(99));
        QVERIFY(RelayProtocol::parseAddress(QStringLiteral("fe80::1"), &host, &port));
        QCOMPARE(host, QStringLiteral("fe80::1"));
        QCOMPARE(port, RelayProtocol::DefaultPort);
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("host:0"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("host:70000"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("host:abc"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral(":1765"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QString(), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("[::1"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("bad:address:here:x"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("host name:1"), &host, &port));
        QVERIFY(!RelayProtocol::parseAddress(QStringLiteral("[relay.org]:1"), &host, &port));
    }

    void testRegistrationReturnsDerivedKey()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(a.connection->key(), RelayProtocol::relayKey(a.identity.certificate));
        QCOMPARE(b.connection->key(), RelayProtocol::relayKey(b.identity.certificate));
        QVERIFY(a.connection->key() != b.connection->key());
        QCOMPARE(server->deviceCount(), 2);

        a.connection->stop();
        QTRY_COMPARE(server->deviceCount(), 1);
    }

    void testConnectByKeyEndToEndTls()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);

        const QString token = linkTunnel(a, b, b.identity.keyTarget());
        QVERIFY(!token.isEmpty());

        RelayTunnelSocket *sa = openTunnel(server.get(), a.identity, token, RelayTunnelSocket::Role::TlsServer, b.identity.keyTarget());
        RelayTunnelSocket *sb = openTunnel(server.get(), b.identity, token, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        QTRY_VERIFY_WITH_TIMEOUT(sa->isEstablished() && sb->isEstablished(), 10000);
        QCOMPARE(sa->peerCertificate(), b.identity.certificate);
        QCOMPARE(sb->peerCertificate(), a.identity.certificate);
        QVERIFY(sa->isEncrypted());

        sa->write("hello from a\n");
        sb->write("hello from b\n");
        QCOMPARE(readLine(sb), QByteArray("hello from a\n"));
        QCOMPARE(readLine(sa), QByteArray("hello from b\n"));
        QCOMPARE(server->tunnelCount(), 1);

        // Closing one end tears the tunnel down
        QSignalSpy disconnected(sb, &QAbstractSocket::disconnected);
        sa->disconnectFromHost();
        QVERIFY(disconnected.wait(5000));
        QTRY_COMPARE(server->tunnelCount(), 0);
        delete sa;
        delete sb;
    }

    void testConnectByFingerprint()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);

        const QString colonFingerprint = QString::fromLatin1(b.identity.certificate.digest(QCryptographicHash::Sha256).toHex(':').toUpper());
        const QString token = linkTunnel(a, b, RelayProtocol::parseTarget(colonFingerprint));
        QVERIFY(!token.isEmpty());

        RelayTunnelSocket *sa = openTunnel(server.get(), a.identity, token, RelayTunnelSocket::Role::TlsServer, b.identity.fingerprintTarget());
        RelayTunnelSocket *sb = openTunnel(server.get(), b.identity, token, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        QTRY_VERIFY_WITH_TIMEOUT(sa->isEstablished() && sb->isEstablished(), 10000);
        delete sa;
        delete sb;
    }

    void testWrongPeerRejected()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);
        const Identity intruder = Identity::generate();

        // Someone else attaches with the token meant for b: a's pin on b's key must reject it
        const QString token = linkTunnel(a, b, b.identity.keyTarget());
        QVERIFY(!token.isEmpty());
        RelayTunnelSocket *sa = openTunnel(server.get(), a.identity, token, RelayTunnelSocket::Role::TlsServer, b.identity.keyTarget());
        RelayTunnelSocket *si = openTunnel(server.get(), intruder, token, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        QSignalSpy failed(sa, &RelayTunnelSocket::failed);
        QSignalSpy established(sa, &RelayTunnelSocket::established);
        QVERIFY(failed.wait(10000));
        QVERIFY(established.isEmpty());
        QTRY_VERIFY(si->state() == QAbstractSocket::UnconnectedState);
        QVERIFY(si->bytesAvailable() == 0);
        delete sa;
        delete si;

        // The receiving side pins too: b refuses a server that isn't the device the relay announced
        const QString token2 = linkTunnel(a, b, b.identity.keyTarget());
        QVERIFY(!token2.isEmpty());
        RelayTunnelSocket *fakeA = openTunnel(server.get(), intruder, token2, RelayTunnelSocket::Role::TlsServer, b.identity.keyTarget());
        RelayTunnelSocket *sb = openTunnel(server.get(), b.identity, token2, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        QSignalSpy failedB(sb, &RelayTunnelSocket::failed);
        QVERIFY(failedB.wait(10000));
        QVERIFY(!sb->isEstablished());
        delete fakeA;
        delete sb;
    }

    void testInvalidTokenAndGarbage()
    {
        auto server = startServer();
        QVERIFY(server);

        QTcpSocket attach;
        attach.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(attach.waitForConnected(5000));
        QSignalSpy closed(&attach, &QAbstractSocket::disconnected);
        attach.write(RelayProtocol::encode({{QStringLiteral("type"), QStringLiteral("attach")}, {QStringLiteral("token"), QStringLiteral("00112233")}}));
        QVERIFY(closed.wait(5000));

        QTcpSocket garbage;
        garbage.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(garbage.waitForConnected(5000));
        QSignalSpy closedGarbage(&garbage, &QAbstractSocket::disconnected);
        garbage.write("GET / HTTP/1.1\r\n\r\n");
        QVERIFY(closedGarbage.wait(5000));

        QTcpSocket badVersion;
        badVersion.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(badVersion.waitForConnected(5000));
        QSignalSpy closedVersion(&badVersion, &QAbstractSocket::disconnected);
        badVersion.write(RelayProtocol::encode({{QStringLiteral("type"), QStringLiteral("hello")}, {QStringLiteral("version"), 99}}));
        QVERIFY(closedVersion.wait(5000));
    }

    void testHandshakeTimeout()
    {
        RelayServerLimits limits;
        limits.handshakeTimeoutMs = 200;
        auto server = startServer(limits);
        QVERIFY(server);
        QTcpSocket idle;
        idle.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(idle.waitForConnected(5000));
        QSignalSpy closed(&idle, &QAbstractSocket::disconnected);
        QVERIFY(closed.wait(5000));
    }

    void testConnectErrors()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        const Identity offline = Identity::generate();

        QCOMPARE(requestError(a, offline.keyTarget()), QStringLiteral("not_found"));
        QCOMPARE(requestError(a, offline.fingerprintTarget()), QStringLiteral("not_found"));
        QCOMPARE(requestError(a, a.identity.keyTarget()), QStringLiteral("self"));
        QCOMPARE(requestError(a, offline.keyTarget(), QStringLiteral("bogus")), QStringLiteral("bad_request"));
    }

    void testRateLimit()
    {
        RelayServerLimits limits;
        limits.connectBurst = 3;
        limits.connectRefillMs = 60000;
        auto server = startServer(limits);
        QVERIFY(server);
        Client a = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        const Identity offline = Identity::generate();
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(requestError(a, offline.keyTarget()), QStringLiteral("not_found"));
        }
        QCOMPARE(requestError(a, offline.keyTarget()), QStringLiteral("rate_limited"));
    }

    void testConnectionsPerIpLimit()
    {
        RelayServerLimits limits;
        limits.maxConnectionsPerIp = 2;
        auto server = startServer(limits);
        QVERIFY(server);
        QTcpSocket s1, s2, s3;
        for (QTcpSocket *s : {&s1, &s2}) {
            s->connectToHost(QHostAddress::LocalHost, server->serverPort());
            QVERIFY(s->waitForConnected(5000));
        }
        QTest::qWait(100);
        s3.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(s3.waitForConnected(5000));
        QSignalSpy closed(&s3, &QAbstractSocket::disconnected);
        QVERIFY(closed.wait(5000));
        QCOMPARE(s1.state(), QAbstractSocket::ConnectedState);
    }

    void testTooManyTunnels()
    {
        RelayServerLimits limits;
        limits.maxTunnelsPerDevice = 2;
        auto server = startServer(limits);
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(requestError(a, b.identity.keyTarget()), QStringLiteral("ok"));
        QCOMPARE(requestError(a, b.identity.keyTarget(), QStringLiteral("payload")), QStringLiteral("ok"));
        QCOMPARE(requestError(a, b.identity.keyTarget()), QStringLiteral("too_many_tunnels"));
        // The limit counts tunnels of the target too
        QCOMPARE(requestError(b, a.identity.keyTarget()), QStringLiteral("too_many_tunnels"));
    }

    void testTokenExpiryAndIdleTimeout()
    {
        RelayServerLimits limits;
        limits.tokenExpiryMs = 300;
        limits.tunnelIdleTimeoutMs = 500;
        auto server = startServer(limits);
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);

        QVERIFY(!linkTunnel(a, b, b.identity.keyTarget()).isEmpty());
        QCOMPARE(server->tunnelCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(server->tunnelCount(), 0, 5000);

        const QString token = linkTunnel(a, b, b.identity.keyTarget());
        RelayTunnelSocket *sa = openTunnel(server.get(), a.identity, token, RelayTunnelSocket::Role::TlsServer, b.identity.keyTarget());
        RelayTunnelSocket *sb = openTunnel(server.get(), b.identity, token, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        QTRY_VERIFY_WITH_TIMEOUT(sa->isEstablished() && sb->isEstablished(), 10000);
        QSignalSpy closed(sa, &QAbstractSocket::disconnected);
        QVERIFY(closed.wait(5000)); // Idle timeout
        QCOMPARE(server->tunnelCount(), 0);
        delete sa;
        delete sb;
    }

    void testReplaced()
    {
        auto server = startServer();
        QVERIFY(server);
        Client first = registerClient(server.get());
        QCOMPARE(first.connection->status(), RelayConnection::Status::Registered);
        QSignalSpy serverError(first.connection.get(), &RelayConnection::serverError);
        Client second = registerClient(server.get(), first.identity);
        QVERIFY(serverError.count() > 0 || serverError.wait(5000));
        QCOMPARE(serverError.at(0).at(0).toString(), QStringLiteral("replaced"));
        QTRY_VERIFY(first.connection->status() != RelayConnection::Status::Registered);
        QCOMPARE(server->deviceCount(), 1);
        first.connection->stop();
    }

    void testPeerOnlineRequiresMutualWatch()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);
        QSignalSpy onlineA(a.connection.get(), &RelayConnection::peerOnline);
        QSignalSpy onlineB(b.connection.get(), &RelayConnection::peerOnline);

        a.connection->setWatchList({RelayProtocol::fingerprint(b.identity.certificate)});
        QVERIFY(!onlineA.wait(300));
        QVERIFY(onlineB.isEmpty());

        b.connection->setWatchList({RelayProtocol::fingerprint(a.identity.certificate)});
        QVERIFY(onlineA.wait(5000));
        QVERIFY(onlineB.count() > 0 || onlineB.wait(5000));
        QCOMPARE(onlineA.at(0).at(0).toString(), RelayProtocol::fingerprint(b.identity.certificate));
        QCOMPARE(onlineA.at(0).at(1).toString(), RelayProtocol::relayKey(b.identity.certificate));
        QCOMPARE(onlineB.at(0).at(0).toString(), RelayProtocol::fingerprint(a.identity.certificate));
    }

    void testReconnectAfterServerRestart()
    {
        auto server = startServer();
        QVERIFY(server);
        const quint16 port = server->serverPort();
        Client a = registerClient(server.get());
        a.connection->testSetRetryInterval(100);
        server.reset();
        QTRY_VERIFY(a.connection->status() == RelayConnection::Status::Offline);

        server = std::make_unique<RelayServer>(m_serverIdentity.certificate, m_serverIdentity.key);
        QVERIFY(server->listen(QHostAddress::LocalHost, port));
        a.connection->retryNow();
        QTRY_VERIFY_WITH_TIMEOUT(a.connection->status() == RelayConnection::Status::Registered, 10000);
    }

    void testPayloadTunnel()
    {
        auto server = startServer();
        QVERIFY(server);
        Client a = registerClient(server.get());
        Client b = registerClient(server.get());
        QCOMPARE(a.connection->status(), RelayConnection::Status::Registered);
        QCOMPARE(b.connection->status(), RelayConnection::Status::Registered);

        QSignalSpy ready(a.connection.get(), &RelayConnection::tunnelReady);
        QSignalSpy incoming(b.connection.get(), &RelayConnection::incoming);
        QVERIFY(!a.connection->requestTunnel(b.identity.fingerprintTarget(), QStringLiteral("payload")).isEmpty());
        QVERIFY(ready.wait(5000));
        QCOMPARE(ready.at(0).at(2).toString(), QStringLiteral("payload"));
        QVERIFY(!incoming.wait(200)); // Payload tunnels are not announced to the target
        const QString token = ready.at(0).at(1).toString();

        const QByteArray data = pattern(1024 * 1024 + 123);
        RelayTunnelSocket *sender = openTunnel(server.get(), a.identity, token, RelayTunnelSocket::Role::TlsServer, b.identity.fingerprintTarget());
        RelayTunnelSocket *receiver = openTunnel(server.get(), b.identity, token, RelayTunnelSocket::Role::TlsClient, a.identity.fingerprintTarget());
        connect(sender, &RelayTunnelSocket::established, sender, [sender, &data]() {
            sender->write(data);
        });
        QCOMPARE(readExactly(receiver, data.size(), 20000), data);
        delete sender;
        delete receiver;
    }

    void testConnectTargets()
    {
        const Identity a = Identity::generate();
        const Identity b = Identity::generate();
        ConnectTargets targets;
        QVERIFY(!targets.add(Target()));
        QVERIFY(targets.isEmpty());
        QVERIFY(targets.add(a.keyTarget()));
        QVERIFY(targets.add(a.keyTarget())); // Replaces, doesn't duplicate
        QVERIFY(!targets.take(b.certificate));
        QVERIFY(targets.take(a.certificate));
        QVERIFY(!targets.take(a.certificate));
        QVERIFY(targets.isEmpty());

        targets.add(b.fingerprintTarget());
        targets.testSetExpiry(50);
        QTest::qWait(100);
        QVERIFY(!targets.take(b.certificate));
        QVERIFY(targets.isEmpty());
    }

    // RelayLinkProvider (using this process' KdeConnectConfig identity) against a scripted peer
    void testLinkProviderEndToEnd()
    {
        auto server = startServer();
        QVERIFY(server);

        RelayLinkProvider provider;
        QVERIFY(!provider.setServer(QStringLiteral("bad:address:here:x")));
        QVERIFY(provider.setServer(QStringLiteral("127.0.0.1:%1").arg(server->serverPort())));
        QCOMPARE(KdeConnectConfig::instance().relayServer(), QStringLiteral("127.0.0.1:%1").arg(server->serverPort()));
        provider.onStart();
        QTRY_VERIFY_WITH_TIMEOUT(provider.status() == RelayConnection::Status::Registered, 10000);
        QCOMPARE(provider.relayKey(), RelayProtocol::relayKey(KdeConnectConfig::instance().certificate()));

        Client peer = registerClient(server.get());
        QCOMPARE(peer.connection->status(), RelayConnection::Status::Registered);
        QSignalSpy linkSpy(&provider, &LinkProvider::connectionReceived);

        // The peer connects to the provider by its relay key, as a user typing it would
        const QString token = [&]() {
            QSignalSpy ready(peer.connection.get(), &RelayConnection::tunnelReady);
            peer.connection->requestTunnel({Target::Key, provider.relayKey()}, QStringLiteral("link"));
            return ready.wait(5000) ? ready.at(0).at(1).toString() : QString();
        }();
        QVERIFY(!token.isEmpty());
        const Target providerPin{Target::Key, provider.relayKey()};
        RelayTunnelSocket *peerSocket = openTunnel(server.get(), peer.identity, token, RelayTunnelSocket::Role::TlsServer, providerPin);
        QTRY_VERIFY_WITH_TIMEOUT(peerSocket->isEstablished(), 10000);

        // Identity exchange inside the tunnel
        NetworkPacket identity(PACKET_TYPE_IDENTITY);
        identity.set(QStringLiteral("deviceId"), peer.identity.deviceId);
        identity.set(QStringLiteral("deviceName"), QStringLiteral("Scripted peer"));
        identity.set(QStringLiteral("deviceType"), QStringLiteral("phone"));
        identity.set(QStringLiteral("protocolVersion"), NetworkPacket::s_protocolVersion);
        identity.set(QStringLiteral("incomingCapabilities"), QStringList{QStringLiteral("kdeconnect.ping")});
        identity.set(QStringLiteral("outgoingCapabilities"), QStringList{QStringLiteral("kdeconnect.ping")});
        peerSocket->write(identity.serialize());

        NetworkPacket providerIdentity;
        QVERIFY(NetworkPacket::unserialize(readLine(peerSocket), &providerIdentity));
        QCOMPARE(providerIdentity.get<QString>(QStringLiteral("deviceId")), KdeConnectConfig::instance().deviceId());

        QVERIFY(linkSpy.count() > 0 || linkSpy.wait(5000));
        auto *link = qobject_cast<RelayDeviceLink *>(linkSpy.at(0).at(0).value<DeviceLink *>());
        QVERIFY(link);
        QCOMPARE(link->deviceId(), peer.identity.deviceId);
        QCOMPARE(link->deviceInfo().certificate, peer.identity.certificate);
        QCOMPARE(link->deviceInfo().name, QStringLiteral("Scripted peer"));
        QCOMPARE(link->provider(), QStringLiteral("relay"));

        // Packets both ways
        NetworkPacket ping(QStringLiteral("kdeconnect.ping"), {{QStringLiteral("message"), QStringLiteral("to peer")}});
        QVERIFY(link->sendPacket(ping));
        NetworkPacket received;
        QVERIFY(NetworkPacket::unserialize(readLine(peerSocket), &received));
        QCOMPARE(received.get<QString>(QStringLiteral("message")), QStringLiteral("to peer"));

        QSignalSpy packetSpy(link, &DeviceLink::receivedPacket);
        peerSocket->write(NetworkPacket(QStringLiteral("kdeconnect.ping"), {{QStringLiteral("message"), QStringLiteral("to provider")}}).serialize());
        QVERIFY(packetSpy.wait(5000));
        QCOMPARE(packetSpy.at(0).at(0).value<NetworkPacket>().get<QString>(QStringLiteral("message")), QStringLiteral("to provider"));

        const Target linkPin = RelayProtocol::targetForCertificate(KdeConnectConfig::instance().certificate());

        // Payload from the link to the peer: header carries a relay token, the peer attaches as TLS client
        const QByteArray outgoingData = pattern(300 * 1000);
        auto buffer = QSharedPointer<QBuffer>::create();
        buffer->setData(outgoingData);
        NetworkPacket share(QStringLiteral("kdeconnect.share.request"), {{QStringLiteral("filename"), QStringLiteral("a.bin")}});
        share.setPayload(buffer, outgoingData.size());
        QVERIFY(link->sendPacket(share));
        NetworkPacket header;
        QVERIFY(NetworkPacket::unserialize(readLine(peerSocket), &header));
        QCOMPARE(header.payloadSize(), qint64(outgoingData.size()));
        const QString outgoingToken = header.payloadTransferInfo().value(QStringLiteral("relayToken")).toString();
        QVERIFY(!outgoingToken.isEmpty());
        QVERIFY(!header.payloadTransferInfo().contains(QStringLiteral("port")));
        RelayTunnelSocket *download = openTunnel(server.get(), peer.identity, outgoingToken, RelayTunnelSocket::Role::TlsClient, linkPin);
        QCOMPARE(readExactly(download, outgoingData.size(), 20000), outgoingData);
        QSignalSpy downloadClosed(download, &QAbstractSocket::disconnected);
        QVERIFY(download->state() == QAbstractSocket::UnconnectedState || downloadClosed.wait(5000));
        delete download;

        // Payload from the peer to the link: the link opens the tunnel and exposes it as the packet payload
        const QByteArray incomingData = pattern(1024 * 1024);
        QSignalSpy payloadReady(peer.connection.get(), &RelayConnection::tunnelReady);
        peer.connection->requestTunnel(linkPin, QStringLiteral("payload"));
        QVERIFY(payloadReady.wait(5000));
        const QString incomingToken = payloadReady.at(0).at(1).toString();
        RelayTunnelSocket *upload = openTunnel(server.get(), peer.identity, incomingToken, RelayTunnelSocket::Role::TlsServer, linkPin);
        connect(upload, &RelayTunnelSocket::established, upload, [upload, &incomingData]() {
            upload->write(incomingData);
        });
        NetworkPacket incomingShare(QStringLiteral("kdeconnect.share.request"), {{QStringLiteral("filename"), QStringLiteral("b.bin")}});
        incomingShare.setPayload(nullptr, incomingData.size());
        incomingShare.setPayloadTransferInfo({{QStringLiteral("relayToken"), incomingToken}});
        // setPayload(nullptr, n) keeps hasPayload() true so serialize() includes the transfer info
        peerSocket->write(incomingShare.serialize());
        QVERIFY(packetSpy.wait(5000));
        const NetworkPacket receivedShare = packetSpy.last().at(0).value<NetworkPacket>();
        QVERIFY(receivedShare.payload());
        QCOMPARE(readExactly(receivedShare.payload().data(), incomingData.size(), 20000), incomingData);
        delete upload;

        // Disconnecting the peer destroys the link
        QSignalSpy destroyed(link, &QObject::destroyed);
        peerSocket->disconnectFromHost();
        QVERIFY(destroyed.wait(5000));
        delete peerSocket;

        provider.onStop();
        QCOMPARE(provider.status(), RelayConnection::Status::Disabled);
        QVERIFY(provider.setServer(QString()));
    }

    // The provider initiates by key and pins it: a device with another certificate answering the tunnel is rejected
    void testLinkProviderPinsInitiatedTarget()
    {
        auto server = startServer();
        QVERIFY(server);
        RelayLinkProvider provider;
        QVERIFY(provider.setServer(QStringLiteral("127.0.0.1:%1").arg(server->serverPort())));
        provider.onStart();
        QTRY_VERIFY_WITH_TIMEOUT(provider.status() == RelayConnection::Status::Registered, 10000);

        Client target = registerClient(server.get());
        QCOMPARE(target.connection->status(), RelayConnection::Status::Registered);
        const Identity intruder = Identity::generate();
        QSignalSpy incoming(target.connection.get(), &RelayConnection::incoming);
        QSignalSpy linkSpy(&provider, &LinkProvider::connectionReceived);
        QVERIFY(provider.connectTo(target.identity.keyTarget()));
        QVERIFY(!provider.connectTo(RelayProtocol::targetForCertificate(KdeConnectConfig::instance().certificate()))); // Self
        QVERIFY(incoming.wait(5000));
        const QString token = incoming.at(0).at(0).toString();

        const Target providerPin{Target::Fingerprint, incoming.at(0).at(1).toString()};
        RelayTunnelSocket *socket = openTunnel(server.get(), intruder, token, RelayTunnelSocket::Role::TlsClient, providerPin);
        QSignalSpy closed(socket, &QAbstractSocket::disconnected);
        QSignalSpy failed(socket, &RelayTunnelSocket::failed);
        QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty() || !failed.isEmpty(), 10000);
        QVERIFY(linkSpy.isEmpty());
        delete socket;

        provider.onStop();
        QVERIFY(provider.setServer(QString()));
    }
};

QTEST_GUILESS_MAIN(RelayTest)

#include "relaytest.moc"
