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

// lin_usb (LindeAPI) part of hw_compare: the same requests to the adapter
// (UsbVendorInterface over libusb) and to FakeLinUsbDevice, side by side.
//
//   linusb-probe    control requests and set-data packets, real vs. fake
//   linusb-engine   a small master schedule on the real adapter: what the
//                   (not open source) LIN engine reports
//   linusb-driver   LindeApiInterface on the real adapter vs. the fake

#include "linusb_compare.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/LindeApiDriver/LindeApiInterface.h"
#include "driver/LindeApiDriver/LindeSharedDevice.h"
#include "driver/UsbVendorInterface/UsbVendorInterface.h"
#include "lindeapi_driver/FakeLinUsbDevice.h"

using namespace std::chrono_literals;

namespace
{

QByteArray hex(const void *data, int len)
{
    return QByteArray(static_cast<const char *>(data), len).toHex(' ');
}

// One side: real adapter or fake. Results as text so the two can be compared.
struct Side
{
    std::function<QByteArray(uint8_t, uint16_t, void *, uint16_t)> in;
    std::function<QByteArray(uint8_t, uint16_t, const void *, uint16_t)> out;
    std::function<QByteArray(const void *, int)> write;
    // Next frame from bulk IN, or empty on timeout
    std::function<std::optional<lin_usb_host_frame_t>(int)> read;
    std::optional<uint16_t> droppedBase[2];
};

QByteArray statusText(UsbVendorInterface::Status s)
{
    switch (s)
    {
    case UsbVendorInterface::Status::Ok:       return "ok";
    case UsbVendorInterface::Status::Timeout:  return "timeout";
    case UsbVendorInterface::Status::NoDevice: return "nodevice";
    case UsbVendorInterface::Status::Error:    break;
    }
    return "STALL";
}

QByteArray resultText(FakeLinUsbDevice::Result r)
{
    switch (r)
    {
    case FakeLinUsbDevice::Result::Ok:       return "ok";
    case FakeLinUsbDevice::Result::Timeout:  return "timeout";
    case FakeLinUsbDevice::Result::NoDevice: return "nodevice";
    case FakeLinUsbDevice::Result::Stall:    break;
    }
    return "STALL";
}

Side realSide(UsbVendorInterface &usb)
{
    return Side{
        .in = [&usb](uint8_t breq, uint16_t wValue, void *data, uint16_t len) {
            return statusText(usb.controlIn(breq, wValue, data, len, 500));
        },
        .out = [&usb](uint8_t breq, uint16_t wValue, const void *data, uint16_t len) {
            return statusText(usb.controlOut(breq, wValue, data, len, 500));
        },
        .write = [&usb](const void *data, int len) { return statusText(usb.bulkWrite(data, len, 500)); },
        .read = [&usb](int timeout) -> std::optional<lin_usb_host_frame_t> {
            lin_usb_host_frame_t f{};
            int n = 0;
            if (usb.bulkRead(&f, sizeof(f), n, timeout) != UsbVendorInterface::Status::Ok || n != sizeof(f))
                return std::nullopt;
            return f;
        },
    };
}

Side fakeSide(FakeLinUsbDevice &dev)
{
    const uint8_t itf = FakeLinUsbDevice::k_interface;
    return Side{
        .in = [&dev, itf](uint8_t breq, uint16_t wValue, void *data, uint16_t len) {
            return resultText(dev.control(true, breq, wValue, itf, data, len));
        },
        .out = [&dev, itf](uint8_t breq, uint16_t wValue, const void *data, uint16_t len) {
            return resultText(dev.control(false, breq, wValue, itf, const_cast<void *>(data), len));
        },
        .write = [&dev](const void *data, int len) {
            return resultText(dev.bulkOut(data, len, 500ms));
        },
        .read = [&dev](int timeout) -> std::optional<lin_usb_host_frame_t> {
            lin_usb_host_frame_t f{};
            int n = 0;
            if (dev.bulkIn(&f, sizeof(f), n, std::chrono::milliseconds(timeout)) != FakeLinUsbDevice::Result::Ok)
                return std::nullopt;
            return f;
        },
    };
}

QByteArray describe(const lin_usb_host_frame_t &f, bool withTime = false)
{
    QByteArray s = QByteArray("echo=") + QByteArray::number(f.echo_id, 16) + " id=" +
                   QByteArray::number(f.lin_id, 16) + " ch=" + QByteArray::number(f.channel) +
                   " dlc=" + QByteArray::number(f.dlc) + " flags=" + QByteArray::number(f.flags, 16) +
                   " data=" + hex(f.data, 8);
    if (withTime)
        s += " t=" + QByteArray::number(f.timestamp_ms);
    return s;
}

// Scheduled bus frames are skipped: their timing differs between the two.
// acksOnly also skips sleep / wakeup events: a stopped CANILFD channel reports
// wakeup pulses it sees, e.g. when the bus config re-inits its UART.
QByteArray readAll(Side &side, int timeout, bool acksOnly = false)
{
    QByteArray s;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeout)
    {
        auto f = side.read(20);
        if (!f)
            continue;
        if (f->echo_id == LIN_USB_ECHO_ID_RX &&
            (acksOnly || !(f->flags & (LIN_USB_FRAME_FLAG_SLEEP | LIN_USB_FRAME_FLAG_WAKEUP))))
            continue;
        s += (s.isEmpty() ? "" : " | ") + describe(*f);
    }
    return s.isEmpty() ? QByteArray("(nothing)") : s;
}

lin_usb_schedule_entry_t entry(uint8_t id, uint8_t dir, uint8_t dlc, uint8_t table = 0)
{
    lin_usb_schedule_entry_t e{};
    e.lin_id = id;
    e.direction = dir;
    e.dlc = dlc;
    e.period_ms = 20;
    e.table_id = table;
    for (int i = 0; i < 8; ++i)
        e.data[i] = static_cast<uint8_t>(0x10 + i);
    return e;
}

lin_usb_host_frame_t setData(uint8_t ch, uint8_t id, uint8_t dlc)
{
    lin_usb_host_frame_t f{};
    f.channel = ch;
    f.lin_id = id;
    f.dlc = dlc;
    for (int i = 0; i < 8; ++i)
        f.data[i] = static_cast<uint8_t>(0xA0 + i);
    return f;
}

lin_usb_bus_config_t busConfig(uint8_t flags)
{
    lin_usb_bus_config_t c{};
    c.baudrate = 19200;
    c.lin_version = LIN_USB_VERSION_2_1;
    c.break_length = 13;
    c.flags = flags;
    return c;
}

lin_usb_mode_t mode(uint8_t m, uint8_t table = 0, uint8_t count = 0)
{
    return lin_usb_mode_t{.mode = m, .table_id = table, .entry_count = count, .reserved = 0};
}

using Step = std::function<QByteArray(Side &)>;

std::vector<std::pair<const char *, Step>> probeSteps()
{
    std::vector<std::pair<const char *, Step>> s;
    const auto out = [&s](const char *name, uint8_t breq, uint16_t wValue, auto value) {
        s.emplace_back(name, [=](Side &side) mutable { return side.out(breq, wValue, &value, sizeof(value)); });
    };
    const auto in = [&s](const char *name, uint8_t breq, uint16_t wValue, uint16_t len, bool showData = true) {
        s.emplace_back(name, [=](Side &side) {
            uint8_t buf[64]{};
            const QByteArray st = side.in(breq, wValue, buf, len);
            // BUS_STATE.dropped counts since the device's last USB reset: compare
            // what was dropped during the probe
            if (breq == LIN_USB_BREQ_BUS_STATE && st == "ok")
            {
                lin_usb_bus_state_t bs{};
                std::memcpy(&bs, buf, sizeof(bs));
                if (!side.droppedBase[wValue & 1])
                    side.droppedBase[wValue & 1] = bs.dropped;
                bs.dropped = static_cast<uint16_t>(bs.dropped - *side.droppedBase[wValue & 1]);
                std::memcpy(buf, &bs, sizeof(bs));
            }
            return showData && st == "ok" ? st + " " + hex(buf, len) : st;
        });
    };
    const auto write = [&s](const char *name, lin_usb_host_frame_t f, int len = sizeof(lin_usb_host_frame_t)) {
        s.emplace_back(name, [=](Side &side) {
            const QByteArray st = side.write(&f, len);
            return st + " -> " + readAll(side, 200);
        });
    };

    in("DEVICE_CONFIG", LIN_USB_BREQ_DEVICE_CONFIG, 0, sizeof(lin_usb_device_config_t));
    in("TIMESTAMP", LIN_USB_BREQ_TIMESTAMP, 0, 4, false);
    in("BUS_STATE ch0", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    in("BUS_STATE ch1", LIN_USB_BREQ_BUS_STATE, 1, sizeof(lin_usb_bus_state_t));
    in("BUS_STATE ch2", LIN_USB_BREQ_BUS_STATE, 2, sizeof(lin_usb_bus_state_t));
    out("HOST_FORMAT", LIN_USB_BREQ_HOST_FORMAT, 0, lin_usb_host_config_t{.byte_order = 0xEF, .reserved = {}});
    out("MODE STOP", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    out("BAUDRATE ch0 master", LIN_USB_BREQ_BAUDRATE, 0, busConfig(LIN_USB_FLAG_MASTER));
    out("BAUDRATE ch2", LIN_USB_BREQ_BAUDRATE, 2, busConfig(LIN_USB_FLAG_MASTER));
    out("SCHEDULE slot 0", LIN_USB_BREQ_SCHEDULE, 0x0000, entry(0x21, 0, 4));
    out("SCHEDULE slot 15", LIN_USB_BREQ_SCHEDULE, 0x0F00, entry(0x22, 1, 4));
    out("SCHEDULE slot 16", LIN_USB_BREQ_SCHEDULE, 0x1000, entry(0x21, 0, 4));
    out("SCHEDULE dlc 0", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x21, 0, 0));
    out("SCHEDULE dlc 9", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x21, 0, 9));
    out("SCHEDULE table 3", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x21, 0, 4, 3));
    out("SCHEDULE table 4", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x21, 0, 4, 4));
    out("SCHEDULE ch2", LIN_USB_BREQ_SCHEDULE, 0x0002, entry(0x21, 0, 4));
    out("FRAME_CONFIG", LIN_USB_BREQ_FRAME_CONFIG, 0, entry(0x21, 0, 4));
    out("FRAME_CONFIG dlc 0", LIN_USB_BREQ_FRAME_CONFIG, 0, entry(0x21, 0, 0));
    out("FRAME_CONFIG table 4", LIN_USB_BREQ_FRAME_CONFIG, 0, entry(0x21, 0, 4, 4));
    out("MODE START table 4", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_START, 4, 1));
    out("MODE PAUSE", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_PAUSE));
    out("MODE 7", LIN_USB_BREQ_MODE, 0, mode(7));
    out("MODE STOP ch1", LIN_USB_BREQ_MODE, 1, mode(LIN_USB_MODE_STOP));
    out("MODE STOP ch2", LIN_USB_BREQ_MODE, 2, mode(LIN_USB_MODE_STOP));
    out("SLEEP_WAKEUP ch2", LIN_USB_BREQ_SLEEP_WAKEUP, 2, lin_usb_sleep_wakeup_t{});
    s.emplace_back("IDENTIFY ch0", [](Side &side) { return side.out(LIN_USB_BREQ_IDENTIFY, 0, nullptr, 0); });
    s.emplace_back("IDENTIFY ch2", [](Side &side) { return side.out(LIN_USB_BREQ_IDENTIFY, 2, nullptr, 0); });
    in("request 10 IN", 10, 0, 4);
    out("request 10 OUT", 10, 0, uint32_t{0});
    // Set data while stopped: ACK only, nothing on the bus
    write("set data 0x21 dlc 4", setData(0, 0x21, 4));
    write("set data dlc 0", setData(0, 0x21, 0));
    write("set data dlc 9", setData(0, 0x21, 9));
    write("set data ch 5", setData(5, 0x21, 4));
    write("set data short", setData(0, 0x21, 4), 10);
    // Started: what the engine accepts
    out("SCHEDULE 0: 0x21 pub", LIN_USB_BREQ_SCHEDULE, 0x0000, entry(0x21, 0, 4));
    out("SCHEDULE 1: 0x23 pub", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x23, 0, 2));
    out("SCHEDULE 2: 0x24 sub", LIN_USB_BREQ_SCHEDULE, 0x0200, entry(0x24, 1, 2));
    out("MODE START 0, 2", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_START, 0, 2));
    in("BUS_STATE started", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    write("started 0x21", setData(0, 0x21, 4));
    write("started 0x23", setData(0, 0x23, 2));
    write("started 0x24 (slot 2)", setData(0, 0x24, 2));
    write("started 0x3A", setData(0, 0x3A, 4));
    write("started ch1 0x21", setData(1, 0x21, 4));
    out("SLEEP", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 0, .reserved = {}});
    s.emplace_back("  events", [](Side &side) { return readAll(side, 200); });
    in("BUS_STATE sleeping", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    out("WAKEUP", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 1, .reserved = {}});
    s.emplace_back("  events", [](Side &side) { return readAll(side, 200); });
    in("BUS_STATE awake", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    out("MODE STOP end", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    in("BUS_STATE stopped", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    // Sleep / wakeup on a stopped channel do nothing (before the firmware fix
    // the master restarted itself and reported mismatched events, see README)
    out("SLEEP stopped", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 0, .reserved = {}});
    s.emplace_back("  events", [](Side &side) { return readAll(side, 300); });
    out("WAKEUP stopped", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 1, .reserved = {}});
    s.emplace_back("  events", [](Side &side) { return readAll(side, 300); });
    in("BUS_STATE after", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    // MODE START after the ignored sleep: no stale go-to-sleep goes out
    out("MODE START again", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_START, 0, 2));
    s.emplace_back("  events", [](Side &side) { return readAll(side, 300); });
    in("BUS_STATE restarted", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    out("MODE STOP final", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    // A stopped master must stay quiet. Frames queued before the stop may
    // still be in the device (the probe alternates between the two sides, so
    // the real schedule ran on while the fake's step was read): count only the
    // ones stamped after it.
    s.emplace_back("  bus after stop", [](Side &side) {
        uint32_t stopTs = 0;
        side.in(LIN_USB_BREQ_TIMESTAMP, 0, &stopTs, sizeof(stopTs));
        int n = 0;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300)
        {
            if (auto f = side.read(20); f && static_cast<int32_t>(f->timestamp_ms - stopTs) > 0)
                ++n;
        }
        return QByteArray::number(n) + " frame(s) sent after the stop";
    });
    in("BUS_STATE end", LIN_USB_BREQ_BUS_STATE, 0, sizeof(lin_usb_bus_state_t));
    return s;
}

bool openReal(UsbVendorInterface &usb)
{
    if (!usb.open(LindeSharedDevice::USB_ID, 0))
    {
        std::fprintf(stderr, "cannot open lin_usb device: %s\n", usb.lastError().c_str());
        return false;
    }
    return true;
}

int runProbe()
{
    UsbVendorInterface usb;
    if (!openReal(usb))
        return 1;
    FakeLinUsbDevice dev(FakeLinUsbDevice::Config::canilfd());
    Side real = realSide(usb);
    Side fake = fakeSide(dev);

    std::printf("drained before start: %s\n", readAll(real, 50).constData());
    int diffs = 0;
    for (auto &[name, step] : probeSteps())
    {
        const QByteArray r = step(real);
        const QByteArray f = step(fake);
        const bool same = r == f;
        diffs += same ? 0 : 1;
        std::printf("%-22s %s %s%s%s\n", name, same ? "same" : "DIFF", r.constData(), same ? "" : "\n                       fake ",
                    same ? "" : f.constData());
    }
    std::printf("%d difference(s)\n", diffs);
    return diffs == 0 ? 0 : 3;
}

// What the real engine does with a master schedule and nobody else on the bus
int runEngine()
{
    UsbVendorInterface usb;
    if (!openReal(usb))
        return 1;
    Side real = realSide(usb);
    const auto collect = [&](int ms) {
        QElapsedTimer t;
        t.start();
        int n = 0;
        while (t.elapsed() < ms)
        {
            if (auto f = real.read(50))
            {
                if (n++ < 12)
                    std::printf("    %s\n", describe(*f, true).constData());
            }
        }
        std::printf("    %d frame(s) in %d ms\n", n, ms);
    };
    const auto ctl = [&](const char *name, uint8_t breq, uint16_t wValue, auto v) {
        std::printf("%s: %s\n", name, real.out(breq, wValue, &v, sizeof(v)).constData());
    };
    const auto busState = [&](uint8_t ch) {
        lin_usb_bus_state_t st{};
        const QByteArray r = real.in(LIN_USB_BREQ_BUS_STATE, ch, &st, sizeof(st));
        std::printf("BUS_STATE ch%d: %s state=%d dropped=%d\n", ch, r.constData(), st.state, st.dropped);
    };

    readAll(real, 50);
    ctl("MODE STOP", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    ctl("BAUDRATE master 19200", LIN_USB_BREQ_BAUDRATE, 0, busConfig(LIN_USB_FLAG_MASTER));
    ctl("SCHEDULE 0: 0x21 publisher", LIN_USB_BREQ_SCHEDULE, 0x0000, entry(0x21, 0, 4));
    ctl("SCHEDULE 1: 0x22 subscriber", LIN_USB_BREQ_SCHEDULE, 0x0100, entry(0x22, 1, 4));
    busState(0);
    ctl("MODE START table 0, 2 entries", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_START, 0, 2));
    collect(300);
    busState(0);
    for (auto [name, f] : {std::pair{"set data 0x21", setData(0, 0x21, 4)},
                           std::pair{"set data 0x22 (subscriber)", setData(0, 0x22, 4)},
                           std::pair{"set data 0x3A (not scheduled)", setData(0, 0x3A, 4)}})
    {
        std::printf("%s: %s\n", name, real.write(&f, sizeof(f)).constData());
        collect(150);
    }
    ctl("SLEEP", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 0, .reserved = {}});
    collect(200);
    busState(0);
    ctl("WAKEUP", LIN_USB_BREQ_SLEEP_WAKEUP, 0, lin_usb_sleep_wakeup_t{.command = 1, .reserved = {}});
    collect(200);
    ctl("MODE STOP", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    collect(100);
    busState(0);

    // Restart and stop again, with the device time of the stop
    const auto now = [&] {
        uint32_t ts = 0;
        real.in(LIN_USB_BREQ_TIMESTAMP, 0, &ts, sizeof(ts));
        return ts;
    };
    ctl("MODE START again", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_START, 0, 2));
    collect(200);
    std::printf("stop at t=%u\n", now());
    ctl("MODE STOP", LIN_USB_BREQ_MODE, 0, mode(LIN_USB_MODE_STOP));
    std::printf("stopped at t=%u\n", now());
    collect(500);
    busState(0);
    return 0;
}

const char k_ldf[] = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 19.2 kbps;
Nodes {
    Master: Gateway, 5 ms, 0.1 ms ;
    Slaves: Seat ;
}
Signals {
    Cmd:     32, 0, Gateway, Seat;
    SeatPos: 32, 0, Seat, Gateway;
}
Frames {
    GatewayCmd: 0x21, Gateway, 4 {
        Cmd, 0;
    }
    SeatStatus: 0x22, Seat, 4 {
        SeatPos, 0;
    }
}
Schedule_tables {
    Normal {
        GatewayCmd delay 20 ms;
        SeatStatus delay 20 ms;
    }
}
)";

// LindeApiInterface open / run / set data / close, real adapter only (the
// fake's engine does not put anything on a bus to compare with)
int runDriver()
{
    QTemporaryDir dir;
    const QString ldf = dir.filePath("hw.ldf");
    QFile f(ldf);
    if (!f.open(QIODevice::WriteOnly))
        return 1;
    f.write(k_ldf);
    f.close();

    if (LindeSharedDevice::enumerateDevices() < 1)
    {
        std::fprintf(stderr, "no lin_usb device\n");
        return 1;
    }
    auto shared = std::make_shared<LindeSharedDevice>();
    LindeApiInterface intf(nullptr, shared, 0);
    MeasurementInterface mi;
    mi.setBusType(BusType::LIN);
    mi.setLinBaudRate(19200);
    mi.setLinNodeMode(LinNodeMode::Master);
    mi.setLinLdfPath(ldf);
    intf.applyConfig(mi);

    QElapsedTimer t;
    t.start();
    intf.open();
    std::printf("open: %s in %lld ms, version %s, state %u\n", intf.isOpen() ? "ok" : "FAILED",
                static_cast<long long>(t.elapsed()), qPrintable(intf.getVersion()), intf.getState());
    if (!intf.isOpen())
        return 1;

    const auto run = [&](int ms) {
        QList<BusMessage> msgs;
        QElapsedTimer rt;
        rt.start();
        while (rt.elapsed() < ms)
            intf.readMessage(msgs, 10);
        for (int i = 0; i < msgs.size() && i < 6; ++i)
        {
            const BusMessage &m = msgs[i];
            std::printf("    id=%02X %s len=%d data=%s err=%X flags=%X t=%lld\n", m.getId(), m.isRX() ? "RX" : "TX",
                        m.getLength(),
                        hex(m.getData(), m.getLength()).constData(), unsigned(m.errorFlags()), m.getFlags(),
                        static_cast<long long>(m.getTimestamp_ms()));
        }
        std::printf("    %lld message(s) in %d ms; rx %d tx %d rxerr %d txerr %d overruns %d\n",
                    static_cast<long long>(msgs.size()), ms, intf.getNumRxFrames(), intf.getNumTxFrames(),
                    intf.getNumRxErrors(), intf.getNumTxErrors(), intf.getNumRxOverruns());
    };
    run(300);

    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(0x21);
    msg.setLength(4);
    for (int i = 0; i < 4; ++i)
        msg.setDataAt(i, static_cast<uint8_t>(0xC0 + i));
    std::printf("sendMessage 0x21 C0 C1 C2 C3\n");
    intf.sendMessage(msg);
    run(200);
    msg.setId(0x3A);
    std::printf("sendMessage 0x3A (not in schedule)\n");
    intf.sendMessage(msg);
    run(200);
    std::printf("setLinScheduleTable(3) (not loaded)\n");
    intf.setLinScheduleTable(3);
    run(200);
    std::printf("state %u\n", intf.getState());
    intf.close();
    std::printf("closed\n");
    return 0;
}

} // namespace

int runLinUsb(const QString &command)
{
    if (command == "linusb-probe")
        return runProbe();
    if (command == "linusb-engine")
        return runEngine();
    if (command == "linusb-driver")
        return runDriver();
    return 2;
}
