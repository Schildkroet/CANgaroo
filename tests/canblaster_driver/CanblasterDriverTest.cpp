/*

  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

// CANBlasterInterface against a CANblaster server on localhost UDP
// (FakeCanblasterServer, transcribed from canblaster.py).

#include <atomic>
#include <chrono>
#include <thread>

#include <QElapsedTimer>
#include <QtTest>

#include "core/BusMessage.h"
#include "driver/CANBlastDriver/CANBlasterInterface.h"

#include "FakeCanblasterServer.h"
#include "support/LogCapture.h"

using Frame = FakeCanblasterServer::Frame;
using namespace std::chrono_literals;

namespace
{

QByteArray payload(const BusMessage &msg)
{
    return QByteArray(reinterpret_cast<const char *>(msg.getData()), msg.getLength());
}

struct Rig
{
    explicit Rig(std::chrono::milliseconds clientTimeout = 5s)
        : server(clientTimeout), intf(nullptr, 0, QStringLiteral("127.0.0.1"), false)
    {
    }
    ~Rig() { intf.close(); }

    // Opens and runs the listener until the server has the heartbeat
    bool openAndSubscribe()
    {
        intf.open();
        if (!intf.isOpen())
            return false;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 2000 && server.liveClients() == 0)
            pump(10ms);
        return server.liveClients() > 0;
    }

    QList<BusMessage> pump(std::chrono::milliseconds duration, int *calls = nullptr)
    {
        QList<BusMessage> all;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < duration.count())
        {
            intf.readMessage(all, 10);
            if (calls)
                ++*calls;
        }
        return all;
    }

    FakeCanblasterServer server;
    CANBlasterInterface intf;
};

} // namespace

class CanblasterDriverTest : public QObject
{
    Q_OBJECT

private slots:
    void init() { logcapture::clear(); }

    void discoversServer();
    void subscribesByHeartbeat();
    void receivesFrames_data();
    void receivesFrames();
    void countsReceivedFrames();
    void staysSubscribed();
    void keepsUpWithFullBus();
    void ignoresOtherDatagrams();
    void waitsInsteadOfSpinning();
    void reopensAfterClose();
};

// The beacon goes to a multicast group; skip where the test machine has no
// multicast route at all.
void CanblasterDriverTest::discoversServer()
{
    FakeCanblasterServer server;
    QVERIFY(server.ok());

    std::atomic<bool> stop{false};
    std::thread beacons([&] {
        QUdpSocket beacon;
        QUdpSocket other;
        while (!stop)
        {
            FakeCanblasterServer::beacon(beacon);
            // Something else in the group that is not a CANblaster server
            other.writeDatagram(QByteArray(R"({"protocol":"other", "version":1})"),
                                QHostAddress(QStringLiteral("239.255.43.21")), 20000);
            std::this_thread::sleep_for(100ms);
        }
    });
    const QList<QString> servers = CANBlasterInterface::discoverServers(800ms);
    stop = true;
    beacons.join();

    if (servers.isEmpty())
        QSKIP("no multicast delivery on this machine");
    QCOMPARE(servers.size(), 1);  // one host, the other sender is not a server
}

// Frames are only sent to a client the server has a heartbeat from.
void CanblasterDriverTest::subscribesByHeartbeat()
{
    Rig rig;
    QVERIFY(rig.server.ok());
    QVERIFY(rig.openAndSubscribe());
}

void CanblasterDriverTest::receivesFrames_data()
{
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("ext");
    QTest::addColumn<bool>("rtr");
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<int>("len");

    QTest::newRow("std data") << 0x123u << false << false << QByteArray("\x11\x22\x33", 3) << 3;
    QTest::newRow("std empty") << 0x7FFu << false << false << QByteArray() << 0;
    QTest::newRow("ext 8") << 0x1ABCDEF0u << true << false << QByteArray("\xA0\xA1\xA2\xA3\xA4\xA5\xA6\xA7", 8) << 8;
    QTest::newRow("std remote") << 0x456u << false << true << QByteArray() << 4;
}

void CanblasterDriverTest::receivesFrames()
{
    QFETCH(uint, id);
    QFETCH(bool, ext);
    QFETCH(bool, rtr);
    QFETCH(QByteArray, data);
    QFETCH(int, len);

    Rig rig;
    QVERIFY(rig.openAndSubscribe());
    rig.server.sendCanFrame(Frame{.id = id, .extended = ext, .rtr = rtr, .len = static_cast<uint8_t>(len), .data = data});
    const QList<BusMessage> msgs = rig.pump(100ms);

    QCOMPARE(msgs.size(), 1);
    const BusMessage &m = msgs.first();
    QCOMPARE(m.getId(), id);
    QCOMPARE(m.isExtended(), ext);
    QCOMPARE(m.isRTR(), rtr);
    QCOMPARE(int(m.getLength()), len);
    if (!rtr)
        QCOMPARE(payload(m), data);
    QVERIFY(m.isRX());
}

void CanblasterDriverTest::countsReceivedFrames()
{
    Rig rig;
    QVERIFY(rig.openAndSubscribe());
    for (int i = 0; i < 5; ++i)
        rig.server.sendCanFrame(Frame{.id = 0x100u + i, .len = 1, .data = QByteArray(1, char(i))});
    const QList<BusMessage> msgs = rig.pump(100ms);

    QCOMPARE(msgs.size(), 5);
    QCOMPARE(rig.intf.getNumRxFrames(), 5);
}

// The server drops a client after client_timeout without a heartbeat; the
// interface's heartbeat (1 s) must keep it subscribed.
void CanblasterDriverTest::staysSubscribed()
{
    Rig rig(1500ms);
    QVERIFY(rig.openAndSubscribe());

    int received = 0;
    for (int i = 0; i < 16; ++i)
    {
        rig.server.sendCanFrame(Frame{.id = 0x200, .len = 1, .data = QByteArray(1, char(i))});
        received += static_cast<int>(rig.pump(250ms).size());
    }
    QCOMPARE(received, 16);
}

// A 1 Mbit/s bus at full load carries about 8000 frames/s; canblaster.py sends
// each one as its own datagram.
void CanblasterDriverTest::keepsUpWithFullBus()
{
    Rig rig;
    QVERIFY(rig.openAndSubscribe());

    constexpr int k_frames = 2000;
    std::atomic<bool> done{false};
    QList<BusMessage> msgs;
    std::thread listener([&] {
        while (!done)
            rig.intf.readMessage(msgs, 10);
    });
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < k_frames; ++i)
    {
        rig.server.sendCanFrame(Frame{.id = 0x300, .len = 2, .data = QByteArray(2, char(i))});
        // 125 us per frame: 8000 frames/s
        while (std::chrono::steady_clock::now() - start < i * 125us)
        {
        }
    }
    std::this_thread::sleep_for(300ms);
    done = true;
    listener.join();

    QCOMPARE(msgs.size(), k_frames);
}

// Only the server's 16-byte frames are CAN frames: anything else arriving on
// the client port (another program, a stray packet) must not show up.
void CanblasterDriverTest::ignoresOtherDatagrams()
{
    Rig rig;
    QVERIFY(rig.openAndSubscribe());

    QUdpSocket stranger;
    QVERIFY(stranger.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0));
    stranger.writeDatagram(FakeCanblasterServer::canFrameBytes(Frame{.id = 0x666, .len = 1, .data = "X"}),
                           QHostAddress(QStringLiteral("127.0.0.1")), 20001);
    QUdpSocket local;
    local.writeDatagram(QByteArray("hello"), QHostAddress(QStringLiteral("127.0.0.1")), 20001);
    rig.server.sendCanFrame(Frame{.id = 0x123, .len = 1, .data = "A"});

    const QList<BusMessage> msgs = rig.pump(150ms);
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs.first().getId(), 0x123u);
}

// readMessage() is called in a loop by the listener thread: with nothing to
// read it has to wait its timeout, not return at once.
void CanblasterDriverTest::waitsInsteadOfSpinning()
{
    Rig rig;
    QVERIFY(rig.openAndSubscribe());

    int calls = 0;
    QList<BusMessage> msgs;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 500)
    {
        rig.intf.readMessage(msgs, 50);
        ++calls;
    }
    // 50 ms per idle call: about 10
    QVERIFY2(calls < 40, qPrintable(QString("%1 calls in 500 ms").arg(calls)));
}

void CanblasterDriverTest::reopensAfterClose()
{
    Rig rig;
    QVERIFY(rig.openAndSubscribe());
    rig.intf.close();
    QVERIFY(!rig.intf.isOpen());

    QVERIFY(rig.openAndSubscribe());
    rig.server.sendCanFrame(Frame{.id = 0x42, .len = 1, .data = "B"});
    const QList<BusMessage> msgs = rig.pump(100ms);
    QCOMPARE(msgs.size(), 1);
}

QTEST_GUILESS_MAIN(CanblasterDriverTest)
#include "CanblasterDriverTest.moc"
