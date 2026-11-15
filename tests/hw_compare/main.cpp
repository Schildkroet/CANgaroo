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
//   slcan_hw_compare probe  <port> | fake:<canable1|canable2|weact|elmue>
//   slcan_hw_compare driver <port> | fake:<...>  [canable1|canable2|weact|elmue]
//   slcan_hw_compare bus    <port> <socketcan if> [canable2|elmue]   (500k / 2M)
//   slcan_hw_compare candle-probe <usb serial>          (gs_usb, vs. fake Elmue G431)
//   slcan_hw_compare candle-bus   <usb serial> <socketcan if>
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
#include <QDateTime>
#include <QElapsedTimer>
#include <QStringList>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/SLCANDriver/SLCANInterface.h"
#include "driver/SerialTransport/QtSerialTransport.h"

#include "candle_driver/FakeGsUsbDevice.h"
#include "driver/CandleApiDriver/CandleApiInterface.h"
#include "driver/CandleApiDriver/CandleSharedDevice.h"
#include "driver/CandleApiDriver/api/candle_usb.h"
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
        // After closing an open channel Elmue forgets its bitrates (can_reset)
        {"open without bitrate", {"O\r"}},
        {"data bitrate Y3",  {"Y3\r"}},
        {"bitrate S7",       {"S7\r"}},
        {"data bitrate Y8",  {"Y8\r"}},
        {"silent open",      {"M1\r", "O\r"}},
        {"frame while silent", {"t1230\r"}},
        {"close silent",     {"C\r"}},
        {"open classic",     {"S6\r", "O\r"}},
        {"fd, no data rate", {"d1230\r"}},
        {"remote frame",     {"r1234\r"}},
        {"id too short",     {"t12\r"}},
        {"data short",       {"t1232AB\r"}},
        {"trailing digits",  {"t1231AB00\r"}},
        {"std id too large", {"t8001AB\r"}},
        {"final close",      {"C\r"}},
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
        mi.setSamplePoint(875);    // as the SocketCAN side: 0.875 / 0.750
        mi.setFdSamplePoint(750);
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

int runBus(const QString &portName, const char *ifname, FakeSlcanDevice::Firmware fw)
{
    const auto mfr = fw == FakeSlcanDevice::Firmware::Elmue ? SLCANInterface::Manufacturer::Elmue
                                                           : SLCANInterface::Manufacturer::CANable;
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
        FakeSlcanDevice dev(fw);
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
        FakeSlcanDevice dev(fw);
        SLCANInterface real(nullptr, 0, portName, true, mfr, QtSerialTransport::factory());
        SLCANInterface fake(nullptr, 1, "fake0", true, mfr, dev.factory());
        MeasurementInterface mi;
        mi.setBitrate(500000);
        mi.setCanFD(true);
        mi.setFdBitrate(2000000);
        mi.setSamplePoint(875);    // as the SocketCAN side: 0.875 / 0.750
        mi.setFdSamplePoint(750);
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

    // Listen only: nothing may reach the wire, and what the driver reports
    // about the attempt must be the same on device and fake.
    {
        FakeSlcanDevice dev(fw);
        SLCANInterface real(nullptr, 0, portName, true, mfr, QtSerialTransport::factory());
        SLCANInterface fake(nullptr, 1, "fake0", true, mfr, dev.factory());
        MeasurementInterface mi;
        mi.setBitrate(500000);
        mi.setCanFD(true);
        mi.setFdBitrate(2000000);
        mi.setSamplePoint(875);    // as the SocketCAN side: 0.875 / 0.750
        mi.setFdSamplePoint(750);
        mi.setListenOnlyMode(true);
        for (SLCANInterface *i : {&real, &fake})
        {
            i->applyConfig(mi);
            i->open();
            i->sendMessage(toMessage(frameCases().first().second));
        }
        QList<BusMessage> rs, fs;
        for (int i = 0; i < 20; ++i)
        {
            real.readMessage(rs, 5);
            fake.readMessage(fs, 1);
        }
        const QList<Frame> wire = can.receive(200);
        const bool same = real.getNumTxErrors() == fake.getNumTxErrors() &&
                          real.getNumTxFrames() == fake.getNumTxFrames() && rs.size() == fs.size();
        diffs += same ? 0 : 1;
        std::printf("listen only        %s wire %lld frame(s); real tx %d err %d shown %lld, fake tx %d err %d shown %lld\n",
                    same ? "same" : "DIFF", static_cast<long long>(wire.size()), real.getNumTxFrames(),
                    real.getNumTxErrors(), static_cast<long long>(rs.size()), fake.getNumTxFrames(),
                    fake.getNumTxErrors(), static_cast<long long>(fs.size()));
        real.close();
        fake.close();
    }
    std::printf("%d difference(s)\n", diffs);
    return diffs == 0 ? 0 : 3;
}


// ── gs_usb (CandleAPI) ──────────────────────────────────────────────────────

struct UsbProbe
{
    const char *name;
    uint8_t request;
    bool in;
    uint16_t value;
    QByteArray out;      // OUT data
    uint16_t inSize;     // IN length
    QList<int> masked;   // byte offsets that differ per board / per call
};

QByteArray words(std::initializer_list<uint32_t> w)
{
    QByteArray out;
    for (uint32_t v : w)
        for (int i = 0; i < 4; ++i)
            out.append(static_cast<char>((v >> (8 * i)) & 0xFF));
    return out;
}

// Requests cangaroo sends, ones the firmware should refuse, and the START
// rules (no timing / timing / while running).
const QList<UsbProbe> &usbProbes()
{
    static const QList<UsbProbe> list = {
        {"reset ch0", 2, false, 0, words({0, 0}), 0, {}},
        {"device config", 5, true, 1, {}, 12, {0, 1, 2}},  // HAL version bytes
        {"bt_const", 4, true, 0, {}, 40, {}},
        {"bt_const_ext", 11, true, 0, {}, 72, {}},
        {"timestamp ch0", 6, true, 0, {}, 4, {0, 1, 2, 3}},
        {"timestamp wValue 1", 6, true, 1, {}, 4, {0, 1, 2, 3}},
        {"state ch0 closed", 14, true, 0, {}, 12, {}},
        {"state ch1 (none)", 14, true, 1, {}, 12, {}},
        {"get termination", 13, true, 0, {}, 4, {}},
        {"berr", 3, false, 0, words({1}), 0, {}},
        {"get user id", 8, true, 0, {}, 4, {}},
        {"set tdc", 18, false, 0, words({0, 0, 0, 2}), 0, {}},
        {"bus off recovery", 32, false, 0, words({0}), 0, {}},
        {"host format", 0, false, 1, words({0xbeef}), 0, {}},
        {"start, no timing", 2, false, 0, words({1, 0x10}), 0, {}},
        {"state after", 14, true, 0, {}, 12, {}},
        {"bittiming brp 0", 1, false, 0, words({1, 138, 20, 1, 0}), 0, {}},
        {"start, bad timing", 2, false, 0, words({1, 0x10}), 0, {}},
        {"state after", 14, true, 0, {}, 12, {}},
        {"bittiming 500k", 1, false, 0, words({1, 138, 20, 1, 2}), 0, {}},
        {"data bittiming 2M", 10, false, 0, words({1, 30, 8, 1, 2}), 0, {}},
        {"start fd + ts", 2, false, 0, words({1, 0x110}), 0, {}},
        {"state running", 14, true, 0, {}, 12, {}},
        {"bittiming while on", 1, false, 0, words({1, 138, 20, 1, 4}), 0, {}},
        {"start while on", 2, false, 0, words({1, 0x10}), 0, {}},
        {"state", 14, true, 0, {}, 12, {}},
        {"reset", 2, false, 0, words({0, 0}), 0, {}},
        {"state after reset", 14, true, 0, {}, 12, {}},
    };
    return list;
}

int runCandleProbe(const QString &serial)
{
    qputenv("CANDLE_SERIAL", serial.toLatin1());
    FakeGsUsbDevice fake(FakeGsUsbDevice::Firmware::ElmueG431, 1, L"fake-gsusb-0");

    wchar_t paths[4][CANDLE_USB_PATH_LEN];
    candle_err_t err{};
    const int n = candle_usb_scan(paths, 4, &err);
    std::wstring realPath;
    for (int i = 0; i < n; ++i)
        if (std::wcsncmp(paths[i], L"usb:", 4) == 0)
            realPath = paths[i];
    if (realPath.empty())
    {
        std::fprintf(stderr, "no gs_usb device with serial %s\n", qPrintable(serial));
        return 1;
    }

    candle_usb_t *real = candle_usb_open(realPath.c_str(), false, &err);
    candle_usb_t *sim = candle_usb_open(L"fake-gsusb-0", false, &err);
    if (!real || !sim)
    {
        std::fprintf(stderr, "cannot open (error %d)\n", int(err));
        return 1;
    }

    int diffs = 0;
    for (const UsbProbe &p : usbProbes())
    {
        QByteArray results[2];
        candle_usb_t *devs[2] = {real, sim};
        for (int d = 0; d < 2; ++d)
        {
            QByteArray buf = p.in ? QByteArray(p.inSize, '\0') : p.out;
            const uint8_t type = (p.in ? 0x80 : 0x00) | 0x40 | 0x01;  // vendor, interface
            const bool ok = candle_usb_control(devs[d], p.request, type, p.value, 0,
                                               buf.data(), static_cast<uint16_t>(buf.size()));
            if (ok && p.in)
                for (int m : p.masked)
                    if (m < buf.size())
                        buf[m] = 'x';
            results[d] = !ok ? QByteArray("STALL") : (p.in ? "ok " + buf.toHex() : QByteArray("ok"));
        }
        const bool same = results[0] == results[1];
        diffs += same ? 0 : 1;
        std::printf("%-20s %s %s%s%s\n", p.name, same ? "same" : "DIFF", results[0].constData(),
                    same ? "" : "  fake ", same ? "" : results[1].constData());
    }
    candle_usb_close(real);
    candle_usb_close(sim);
    std::printf("%d difference(s)\n", diffs);
    return diffs == 0 ? 0 : 3;
}

FakeGsUsbDevice::Frame toGs(const Frame &f)
{
    return FakeGsUsbDevice::Frame{.id = f.id, .extended = f.extended, .rtr = f.rtr, .fd = f.fd,
                                  .brs = f.brs, .dlc = f.dlc, .data = f.data};
}

QByteArray describeMsg(const BusMessage &m)
{
    Frame f;
    f.id = m.getId();
    f.extended = m.isExtended();
    f.rtr = m.isRTR();
    f.fd = m.isFD();
    f.brs = m.isBRS();
    int dlc = 0;
    while (dlc < 15 && k_dlcToBytes[dlc] < m.getLength())
        ++dlc;
    f.dlc = static_cast<uint8_t>(m.isRTR() || !m.isFD() ? m.getLength() : dlc);
    if (!m.isRTR())
        f.data = QByteArray(reinterpret_cast<const char *>(m.getData()), m.getLength());
    return (m.isRX() ? "RX " : "TX ") + describe(f);
}

// Leaves channel 0 running at 250 kbit/s, as another program would.
int runCandleLeave(const QString &serial)
{
    qputenv("CANDLE_SERIAL", serial.toLatin1());
    wchar_t paths[4][CANDLE_USB_PATH_LEN];
    candle_err_t err{};
    const int n = candle_usb_scan(paths, 4, &err);
    for (int i = 0; i < n; ++i)
    {
        if (std::wcsncmp(paths[i], L"usb:", 4) != 0)
            continue;
        candle_usb_t *usb = candle_usb_open(paths[i], false, &err);
        if (!usb)
            return 1;
        const uint8_t out = 0x41;
        QByteArray reset = words({0, 0}), timing = words({1, 138, 20, 1, 4}), start = words({1, 0});
        candle_usb_control(usb, 2, out, 0, 0, reset.data(), 8);
        candle_usb_control(usb, 1, out, 0, 0, timing.data(), 20);  // 160 MHz / (4 * 160)
        candle_usb_control(usb, 2, out, 0, 0, start.data(), 8);
        QByteArray state(12, '\0');
        candle_usb_control(usb, 14, 0xC1, 0, 0, state.data(), 12);
        std::printf("left running at 250 kbit/s, state %s\n", state.toHex().constData());
        candle_usb_close(usb);
        return 0;
    }
    return 1;
}

int runCandleBus(const QString &serial, const char *ifname)
{
    qputenv("CANDLE_SERIAL", serial.toLatin1());
    CanSocket can(ifname);
    if (!can.ok())
        return 1;
    FakeGsUsbDevice fake(FakeGsUsbDevice::Firmware::ElmueG431, 1, L"fake-gsusb-0");

    std::shared_ptr<CandleSharedDevice> realDev, fakeDev;
    for (const CandleDiscovered &d : candleDiscoverDevices())
    {
        const std::wstring path(candle_dev_get_path(d.device->handle));
        (path.rfind(L"usb:", 0) == 0 ? realDev : fakeDev) = d.device;
    }
    if (!realDev || !fakeDev)
    {
        std::fprintf(stderr, "device not found\n");
        return 1;
    }
    const auto clock = [] {
        return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()) * 1000u;
    };
    CandleApiInterface real(nullptr, realDev, 0, clock);
    CandleApiInterface simCh0(nullptr, fakeDev, 0, clock);

    MeasurementInterface mi;
    mi.setBitrate(500000);
    mi.setSamplePoint(875);
    mi.setCanFD(true);
    mi.setFdBitrate(2000000);
    mi.setFdSamplePoint(800);  // the closest the driver offers to the bus's 750
    for (CandleApiInterface *i : {&real, &simCh0})
    {
        i->applyConfig(mi);
        i->open();
    }
    std::printf("open: real %s, fake %s\n", real.isOpen() ? "ok" : "FAILED", simCh0.isOpen() ? "ok" : "FAILED");
    if (!real.isOpen() || !simCh0.isOpen())
        return 1;
    (void)can.receive(100);

    const auto collect = [](CandleApiInterface &i) {
        QList<BusMessage> all;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 200)
            i.readMessage(all, 10);
        return all;
    };

    int diffs = 0;
    for (const auto &[name, frame] : frameCases())
    {
        can.send(frame);
        fake.receiveBusFrame(0, toGs(frame));
        const QList<BusMessage> r = collect(real);
        const QList<BusMessage> f = collect(simCh0);
        const QByteArray rs = r.isEmpty() ? QByteArray("(nothing)") : describeMsg(r.first());
        const QByteArray fs = f.isEmpty() ? QByteArray("(nothing)") : describeMsg(f.first());
        const bool same = rs == fs && r.size() == f.size();
        diffs += same ? 0 : 1;
        std::printf("RX %-14s %s %s%s%s\n", name, same ? "same" : "DIFF", rs.constData(),
                    same ? "" : "  fake ", same ? "" : fs.constData());
    }
    for (const auto &[name, frame] : frameCases())
    {
        real.sendMessage(toMessage(frame));
        simCh0.sendMessage(toMessage(frame));
        const QList<BusMessage> rEcho = collect(real);
        const QList<BusMessage> fEcho = collect(simCh0);
        const QList<Frame> wire = can.receive(200);
        const QByteArray w = wire.isEmpty() ? QByteArray("(nothing)") : describe(wire.first());
        const auto onBus = fake.transmitted(0);
        const QByteArray fw = onBus.isEmpty() ? QByteArray("(nothing)") : describe(Frame{
            .id = onBus.last().id, .extended = onBus.last().extended, .rtr = onBus.last().rtr,
            .fd = onBus.last().fd, .brs = onBus.last().brs, .dlc = onBus.last().dlc, .data = onBus.last().data});
        const bool same = w == fw && rEcho.size() == fEcho.size();
        diffs += same ? 0 : 1;
        std::printf("TX %-14s %s wire %s, shown %lld%s%s\n", name, same ? "same" : "DIFF", w.constData(),
                    static_cast<long long>(rEcho.size()), same ? "" : "  fake ", same ? "" : fw.constData());
    }
    real.close();
    simCh0.close();
    std::printf("%d difference(s)\n", diffs);
    return diffs == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == "candle-probe")
        return runCandleProbe(args[2]);
    if (args.size() >= 3 && args[1] == "candle-leave")
        return runCandleLeave(args[2]);
    if (args.size() >= 4 && args[1] == "candle-bus")
        return runCandleBus(args[2], qPrintable(args[3]));

    if (args.size() >= 4 && args[1] == "bus")
    {
        const bool elmue = args.size() > 4 && args[4] == "elmue";
        return runBus(args[2], qPrintable(args[3]),
                      elmue ? FakeSlcanDevice::Firmware::Elmue : FakeSlcanDevice::Firmware::CANable2);
    }

    if (args.size() < 3 || (args[1] != "probe" && args[1] != "driver"))
    {
        std::fprintf(stderr, "usage: %s probe|driver <port>|fake:<canable1|canable2|weact> [board]\n",
                     qPrintable(args[0]));
        return 2;
    }

    const auto firmwareFor = [](const QString &n) {
        if (n == "canable1") return FakeSlcanDevice::Firmware::CANable1;
        if (n == "weact")    return FakeSlcanDevice::Firmware::WeAct;
        if (n == "elmue")    return FakeSlcanDevice::Firmware::Elmue;
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
