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

// Checks a simulated SLCAN adapter against the real one.
//
//   slcan_hw_compare probe  <port> | fake:<canable1|canable2|weact>
//   slcan_hw_compare driver <port> | fake:<...>  [canable1|canable2|weact]
//   slcan_hw_compare bus    <port> <socketcan if>   (CANable 2, 500k / 2M)
//
// "probe" sends a fixed list of command scripts and prints every reply byte;
// run it once against the device and once against the fake and diff the two.
// "driver" runs SLCANInterface itself through open, TX and close.
// "bus" needs a SocketCAN channel on the same bus: frames sent there must come
// out of the device as the fake reports them, and frames SLCANInterface sends
// must reach the wire as they reach the fake.
// Needs hardware, so it is not part of "make check".

#include <chrono>
#include <cstdio>
#include <cstring>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/SLCANDriver/SLCANInterface.h"
#include "driver/SerialTransport/QtSerialTransport.h"

#include "slcan_driver/FakeSlcanDevice.h"

using namespace std::chrono_literals;

namespace
{

QByteArray escaped(const QByteArray &bytes)
{
    QByteArray out;
    for (char c : bytes)
    {
        if (c == '\r')
            out += "\\r";
        else if (c == '\x07')
            out += "\\a";
        else if (c >= 0x20 && c < 0x7F)
            out += c;
        else
            out += QByteArray("\\x") + QByteArray::number(static_cast<uint8_t>(c), 16).rightJustified(2, '0');
    }
    return out;
}

// Everything the device sends until it has been quiet for 150 ms
QByteArray collect(SerialTransport &port)
{
    QByteArray all = port.readAll();
    while (port.waitForReadyRead(150ms))
        all += port.readAll();
    return all + port.readAll();
}

struct Probe
{
    const char *name;
    QList<QByteArray> writes;  // each one written on its own, reply collected after it
};

// Commands of the SLCAN dialects in FakeSlcanDevice, including ones a
// firmware should refuse. "t..." while open has no bus partner here, so it can
// only show what the host sees, not what happens on the wires.
const QList<Probe> &probes()
{
    static const QList<Probe> list = {
        {"close",            {"C\r"}},
        {"error register",   {"E\r"}},
        {"version",          {"V\r"}},
        {"empty line",       {"\r"}},
        {"unknown command",  {"Q\r"}},
        {"bitrate S6",       {"S6\r"}},
        {"bitrate S9",       {"S9\r"}},
        {"bitrate SD",       {"SD\r"}},
        {"bitrate custom",   {"S023407\r"}},
        {"data bitrate Y2",  {"Y2\r"}},
        {"data bitrate Y1",  {"Y1\r"}},
        {"mode M1",          {"M1\r"}},
        {"mode M0",          {"M0\r"}},
        {"auto retry A1",    {"A1\r"}},
        {"binary mode H0",   {"H0\r"}},
        {"frame while closed", {"t1230\r"}},
        {"error after closed TX", {"E\r"}},
        {"open",             {"O\r"}},
        {"open twice",       {"O\r"}},
        {"mode while open",  {"M1\r"}},
        {"bitrate while open", {"S4\r"}},
        {"version while open", {"V\r"}},
        {"frame, no partner", {"t1232ABCD\r"}},
        {"error after open TX", {"E\r"}},
        {"short after long", {"t7FF0\r", "\r"}},
        {"close again",      {"C\r"}},
        {"batched config",   {"S6\rM0\rO\rC\r"}},
    };
    return list;
}

int runProbes(SerialTransport &port)
{
    port.clear();
    for (const Probe &probe : probes())
    {
        QByteArray reply;
        for (const QByteArray &w : probe.writes)
        {
            port.write(w);
            port.waitForBytesWritten(50ms);
            reply += collect(port);
        }
        std::printf("%-22s -> \"%s\"\n", probe.name, escaped(reply).constData());
    }
    return 0;
}

int runDriver(SerialTransportFactory factory, const QString &name, SLCANInterface::Manufacturer mfr, bool fd)
{
    SLCANInterface intf(nullptr, 0, name, fd, mfr, std::move(factory));

    MeasurementInterface mi;
    mi.setBitrate(500000);
    if (fd)
    {
        mi.setCanFD(true);
        mi.setFdBitrate(2000000);
    }
    intf.applyConfig(mi);

    QElapsedTimer t;
    t.start();
    intf.open();
    std::printf("open             -> %s in %lld ms, version \"%s\"\n", intf.isOpen() ? "ok" : "FAILED",
                static_cast<long long>(t.elapsed()), intf.getVersion().toLatin1().constData());
    if (!intf.isOpen())
        return 1;

    // No bus partner: what the driver reports is all the host can see.
    BusMessage msg;
    msg.setId(0x123);
    msg.setLength(2);
    msg.setByte(0, 0xAB);
    msg.setByte(1, 0xCD);
    intf.sendMessage(msg);

    QList<BusMessage> got;
    t.restart();
    while (t.elapsed() < 300)
        intf.readMessage(got, 10);
    int tx = 0;
    int rx = 0;
    for (const BusMessage &m : std::as_const(got))
        (m.isRX() ? rx : tx)++;
    std::printf("send 0x123       -> %d TX, %d RX reported; tx frames %d, tx errors %d, state \"%s\"\n",
                tx, rx, intf.getNumTxFrames(), intf.getNumTxErrors(),
                intf.getStateText().toLatin1().constData());

    intf.close();
    std::printf("close            -> %s\n", intf.isOpen() ? "still open" : "closed");
    return 0;
}


// ── bus mode ────────────────────────────────────────────────────────────────

using Frame = FakeSlcanDevice::Frame;

constexpr uint8_t k_dlcToBytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

QByteArray counting(int n, int start = 0)
{
    QByteArray out;
    for (int i = 0; i < n; ++i)
        out.append(static_cast<char>(start + i));
    return out;
}

// Same cases as tests/slcan_driver
const QList<QPair<const char *, Frame>> &frameCases()
{
    static const QList<QPair<const char *, Frame>> cases = {
        {"std data", Frame{.id = 0x123, .dlc = 3, .data = QByteArray("\x11\x22\x33", 3)}},
        {"std empty", Frame{.id = 0x7FF, .dlc = 0}},
        {"ext data", Frame{.id = 0x1ABCDEF0, .extended = true, .dlc = 8, .data = counting(8, 0xA0)}},
        {"std remote", Frame{.id = 0x456, .rtr = true, .dlc = 4}},
        {"ext remote", Frame{.id = 0x18DAF110, .extended = true, .rtr = true, .dlc = 8}},
        {"fd 12", Frame{.id = 0x321, .fd = true, .dlc = 9, .data = counting(12)}},
        {"fd brs 64", Frame{.id = 0x100, .fd = true, .brs = true, .dlc = 0xF, .data = counting(64)}},
        {"fd ext brs 64", Frame{.id = 0x1FFFFFFF, .extended = true, .fd = true, .brs = true, .dlc = 0xF,
                                .data = counting(64, 0x40)}},
    };
    return cases;
}

class CanSocket
{
public:
    explicit CanSocket(const char *ifname)
    {
        _fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
        const int on = 1;
        ::setsockopt(_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on));
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = static_cast<int>(if_nametoindex(ifname));
        _ok = addr.can_ifindex != 0 && ::bind(_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    }
    ~CanSocket() { ::close(_fd); }
    [[nodiscard]] bool ok() const { return _ok; }

    void send(const Frame &f)
    {
        canid_t id = f.id | (f.extended ? CAN_EFF_FLAG : 0) | (f.rtr ? CAN_RTR_FLAG : 0);
        if (f.fd)
        {
            canfd_frame cf{};
            cf.can_id = id;
            cf.len = k_dlcToBytes[f.dlc];
            cf.flags = f.brs ? CANFD_BRS : 0;
            std::memcpy(cf.data, f.data.constData(), f.data.size());
            (void)::write(_fd, &cf, sizeof(cf));
        }
        else
        {
            can_frame cf{};
            cf.can_id = id;
            cf.len = f.dlc;
            std::memcpy(cf.data, f.data.constData(), f.data.size());
            (void)::write(_fd, &cf, sizeof(cf));
        }
    }

    QList<Frame> receive(int waitMs)
    {
        QList<Frame> out;
        pollfd p{_fd, POLLIN, 0};
        while (::poll(&p, 1, waitMs) > 0)
        {
            canfd_frame cf{};
            const ssize_t n = ::read(_fd, &cf, sizeof(cf));
            Frame f;
            f.extended = cf.can_id & CAN_EFF_FLAG;
            f.rtr = cf.can_id & CAN_RTR_FLAG;
            f.id = cf.can_id & (f.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
            f.fd = n == CANFD_MTU;
            f.brs = f.fd && (cf.flags & CANFD_BRS);
            int dlc = 0;
            while (dlc < 15 && k_dlcToBytes[dlc] < cf.len)
                ++dlc;
            f.dlc = static_cast<uint8_t>(f.rtr ? cf.len : dlc);
            if (!f.rtr)
                f.data = QByteArray(reinterpret_cast<const char *>(cf.data), cf.len);
            out.append(f);
            waitMs = 50;
        }
        return out;
    }

private:
    int _fd{-1};
    bool _ok{false};
};

QByteArray describe(const Frame &f)
{
    return QByteArray::number(f.id, 16) + (f.extended ? " ext" : "") + (f.rtr ? " rtr" : "") +
           (f.fd ? " fd" : "") + (f.brs ? " brs" : "") + " dlc " + QByteArray::number(f.dlc) + " [" +
           f.data.toHex() + "]";
}

BusMessage toMessage(const Frame &f)
{
    BusMessage msg;
    msg.setId(f.id);
    msg.setExtended(f.extended);
    msg.setRTR(f.rtr);
    msg.setFD(f.fd);
    msg.setBRS(f.brs);
    msg.setLength(k_dlcToBytes[f.dlc]);
    for (int i = 0; i < f.data.size(); ++i)
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(f.data.at(i)));
    return msg;
}

int runBus(const QString &portName, const char *ifname)
{
    CanSocket can(ifname);
    if (!can.ok())
    {
        std::fprintf(stderr, "cannot bind %s\n", ifname);
        return 1;
    }
    int diffs = 0;
    const QByteArray setup = "C\rS6\rY2\rM0\rO\r";

    // RX: the line the device prints for a frame from the bus
    {
        auto real = QtSerialTransport::factory()();
        if (!real->open(portName, SerialTransport::Settings{.baudRate = 1000000}))
            return 1;
        FakeSlcanDevice dev(FakeSlcanDevice::Firmware::CANable2);
        auto fake = dev.factory()();
        (void)fake->open("fake0", {});
        real->clear();
        for (SerialTransport *p : {real.get(), fake.get()})
        {
            p->write(setup);
            p->waitForBytesWritten(50ms);
            (void)collect(*p);
        }
        for (const auto &[name, frame] : frameCases())
        {
            can.send(frame);
            const QByteArray r = collect(*real);
            dev.receiveBusFrame(frame);
            const QByteArray f = collect(*fake);
            // A remote frame's data digits are undefined on CANable (stale RX
            // FIFO bytes): compare up to the DLC only.
            const int head = 1 + (frame.extended ? 8 : 3) + 1;
            const bool same = frame.rtr ? (r.left(head) == f.left(head) && r.size() == f.size()) : r == f;
            diffs += same ? 0 : 1;
            std::printf("RX %-14s %s real \"%s\"%s%s\n", name, same ? "same" : "DIFF", escaped(r).constData(),
                        same ? "" : "  fake \"", same ? "" : (escaped(f) + "\"").constData());
        }
        real->write("C\r");
        real->waitForBytesWritten(50ms);
    }

    // TX: what SLCANInterface puts on the wire, on the device and on the fake
    {
        FakeSlcanDevice dev(FakeSlcanDevice::Firmware::CANable2);
        SLCANInterface real(nullptr, 0, portName, true, SLCANInterface::Manufacturer::CANable,
                            QtSerialTransport::factory());
        SLCANInterface fake(nullptr, 1, "fake0", true, SLCANInterface::Manufacturer::CANable, dev.factory());
        MeasurementInterface mi;
        mi.setBitrate(500000);
        mi.setCanFD(true);
        mi.setFdBitrate(2000000);
        for (SLCANInterface *i : {&real, &fake})
        {
            i->applyConfig(mi);
            i->open();
        }
        (void)can.receive(100);

        for (const auto &[name, frame] : frameCases())
        {
            QList<BusMessage> sink;
            real.sendMessage(toMessage(frame));
            fake.sendMessage(toMessage(frame));
            for (int i = 0; i < 20; ++i)
            {
                real.readMessage(sink, 5);
                fake.readMessage(sink, 1);
            }
            const QList<Frame> wire = can.receive(200);
            const QByteArray w = wire.isEmpty() ? QByteArray("(nothing)") : describe(wire.first());
            const QByteArray f = dev.transmitted().isEmpty() ? QByteArray("(nothing)")
                                                             : describe(dev.transmitted().last());
            const QByteArray want = describe(frame);
            const bool same = w == f && wire.size() == 1;
            diffs += same ? 0 : 1;
            std::printf("TX %-14s %s wire %s%s%s%s\n", name, same ? "same" : "DIFF", w.constData(),
                        w == want ? "" : "  (sent " , w == want ? "" : want.constData(), w == want ? "" : ")");
            if (!same)
                std::printf("   fake %s, %lld frame(s) on the wire\n", f.constData(),
                            static_cast<long long>(wire.size()));
        }
        real.close();
        fake.close();
    }
    std::printf("%d difference(s)\n", diffs);
    return diffs == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 4 && args[1] == "bus")
        return runBus(args[2], qPrintable(args[3]));

    if (args.size() < 3 || (args[1] != "probe" && args[1] != "driver"))
    {
        std::fprintf(stderr, "usage: %s probe|driver <port>|fake:<canable1|canable2|weact> [board]\n",
                     qPrintable(args[0]));
        return 2;
    }

    const auto firmwareFor = [](const QString &n) {
        if (n == "canable1") return FakeSlcanDevice::Firmware::CANable1;
        if (n == "weact")    return FakeSlcanDevice::Firmware::WeAct;
        return FakeSlcanDevice::Firmware::CANable2;
    };

    const QString target = args[2];
    const bool fake = target.startsWith("fake:");
    const QString board = fake ? target.mid(5) : (args.size() > 3 ? args[3] : QStringLiteral("canable2"));
    const FakeSlcanDevice::Firmware fw = firmwareFor(board);

    FakeSlcanDevice device(fw);
    SerialTransportFactory factory = fake ? device.factory() : QtSerialTransport::factory();

    if (args[1] == "driver")
    {
        const auto mfr = fw == FakeSlcanDevice::Firmware::WeAct ? SLCANInterface::Manufacturer::WeActStudio
                                                               : SLCANInterface::Manufacturer::CANable;
        return runDriver(std::move(factory), fake ? QStringLiteral("fake0") : target, mfr,
                         fw != FakeSlcanDevice::Firmware::CANable1);
    }

    auto port = factory();
    if (!port->open(target, SerialTransport::Settings{.baudRate = 1000000}))
    {
        std::fprintf(stderr, "cannot open %s: %s\n", qPrintable(target), qPrintable(port->errorString()));
        return 1;
    }
    return runProbes(*port);
}
