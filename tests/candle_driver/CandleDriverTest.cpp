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

// The CandleAPI driver (candleDiscoverDevices, CandleApiInterface,
// CandleSharedDevice and the gs_usb protocol in api/candle.c) against
// simulated gs_usb adapters (FakeGsUsbDevice). What a device does with a
// request comes from its firmware sources, see FakeGsUsbDevice.h.

#include <chrono>
#include <memory>
#include <thread>

#include <QElapsedTimer>
#include <QSet>
#include <QtTest>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/CandleApiDriver/CandleApiInterface.h"
#include "driver/CandleApiDriver/CandleSharedDevice.h"

#include "FakeGsUsbDevice.h"
#include "support/LogCapture.h"

using Fw = FakeGsUsbDevice::Firmware;
using Frame = FakeGsUsbDevice::Frame;

Q_DECLARE_METATYPE(FakeGsUsbDevice::Firmware)

namespace
{

const char *fwName(Fw fw)
{
    return fw == Fw::CandleLightF072 ? "candleLight F072" : "Elmue G431";
}

constexpr uint8_t k_dlcToBytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

uint64_t hostClockUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A device and the interfaces SLCANDriver... CandleApiDriver::update() makes for it
struct Rig
{
    explicit Rig(Fw fw, int channels = 1) : dev(fw, channels)
    {
        for (const CandleDiscovered &found : candleDiscoverDevices())
        {
            for (uint8_t ch = 0; ch < found.channels; ch++)
                intf.push_back(std::make_unique<CandleApiInterface>(nullptr, found.device, ch, hostClockUs));
        }
    }

    ~Rig()
    {
        for (auto &i : intf)
            if (i->isOpen())
                i->close();
        intf.clear();  // releases the shared device before the fake goes
    }

    MeasurementInterface config() const
    {
        MeasurementInterface mi;
        mi.setBitrate(500000);
        mi.setSamplePoint(875);
        if (dev.firmware() == Fw::ElmueG431)
        {
            mi.setCanFD(true);
            mi.setFdBitrate(2000000);
            mi.setFdSamplePoint(800);
        }
        return mi;
    }

    CandleApiInterface &operator[](int ch) { return *intf.at(static_cast<size_t>(ch)); }

    void open(int ch, const MeasurementInterface &mi)
    {
        intf.at(static_cast<size_t>(ch))->applyConfig(mi);
        intf.at(static_cast<size_t>(ch))->open();
    }
    void open(int ch = 0) { open(ch, config()); }

    // Messages channel `ch` reports within `ms`
    QList<BusMessage> pump(int ch = 0, int ms = 150)
    {
        QList<BusMessage> all;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            intf.at(static_cast<size_t>(ch))->readMessage(all, 10);
        return all;
    }

    FakeGsUsbDevice dev;  // outlives the interfaces
    std::vector<std::unique_ptr<CandleApiInterface>> intf;
};

struct FrameCase
{
    const char *name;
    Frame frame;
    bool needsFd;
};

QByteArray counting(int n, int start = 0)
{
    QByteArray out;
    for (int i = 0; i < n; ++i)
        out.append(static_cast<char>(start + i));
    return out;
}

const QList<FrameCase> &frameCases()
{
    static const QList<FrameCase> cases = {
        {"std data", Frame{.id = 0x123, .dlc = 3, .data = QByteArray("\x11\x22\x33", 3)}, false},
        {"std empty", Frame{.id = 0x7FF, .dlc = 0}, false},
        {"ext data", Frame{.id = 0x1ABCDEF0, .extended = true, .dlc = 8, .data = counting(8, 0xA0)}, false},
        {"std remote", Frame{.id = 0x456, .rtr = true, .dlc = 4}, false},
        {"fd 12", Frame{.id = 0x321, .fd = true, .dlc = 9, .data = counting(12)}, true},
        {"fd brs 64", Frame{.id = 0x100, .fd = true, .brs = true, .dlc = 0xF, .data = counting(64)}, true},
        {"fd ext brs 64", Frame{.id = 0x1FFFFFFF, .extended = true, .fd = true, .brs = true, .dlc = 0xF,
                                .data = counting(64, 0x40)}, true},
    };
    return cases;
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

QByteArray payload(const BusMessage &msg)
{
    return QByteArray(reinterpret_cast<const char *>(msg.getData()), msg.getLength());
}

void addFirmwareRows()
{
    QTest::addColumn<Fw>("fw");
    QTest::newRow(fwName(Fw::CandleLightF072)) << Fw::CandleLightF072;
    QTest::newRow(fwName(Fw::ElmueG431)) << Fw::ElmueG431;
}

// Bitrate and sample point (permille) a timing gives at `clock`
std::pair<double, int> rateOf(const FakeGsUsbDevice::Timing &t, uint32_t clock)
{
    const double rate = double(clock) / (double(t.brp) * t.quanta());
    const int sp = static_cast<int>(std::lround(1000.0 * (1 + t.prop + t.seg1) / t.quanta()));
    return {rate, sp};
}

} // namespace

class CandleDriverTest : public QObject
{
    Q_OBJECT

private slots:
    void init() { logcapture::clear(); }

    // discovery
    void discoversDevice_data() { addFirmwareRows(); }
    void discoversDevice();
    void skipsDeviceInUse();

    // open()
    void opensWithoutRefusedRequests_data() { addFirmwareRows(); }
    void opensWithoutRefusedRequests();
    void offeredBitratesAreWhatTheDeviceRuns_data();
    void offeredBitratesAreWhatTheDeviceRuns();
    void deviceLeftRunningIsReconfigured_data() { addFirmwareRows(); }
    void deviceLeftRunningIsReconfigured();
    void reopensAfterClose_data() { addFirmwareRows(); }
    void reopensAfterClose();
    void closedChannelDoesNotTouchDevice_data() { addFirmwareRows(); }
    void closedChannelDoesNotTouchDevice();

    // frames
    void transmitsFrames_data();
    void transmitsFrames();
    void receivesFrames_data();
    void receivesFrames();
    void elmueListenOnlyNeverSends();
    void reportsDeviceOverflow();

    // bus state
    void upstreamReportsErrorWarning();
    void upstreamBusOffAndHostRecovery();
    void elmueHasNoHostRecovery();

    // multi channel / lifecycle
    void twoChannelsShareTheDevice();
    void closeWithoutOpenKeepsOtherChannel();
    void unplugDoesNotSpin_data() { addFirmwareRows(); }
    void unplugDoesNotSpin();
};


// ── Discovery ────────────────────────────────────────────────────────────────

void CandleDriverTest::discoversDevice()
{
    QFETCH(Fw, fw);
    Rig rig(fw);

    QCOMPARE(rig.intf.size(), size_t(1));
    QCOMPARE(rig[0].getName(), QStringLiteral("candle0_ch0"));
    QVERIFY(!rig[0].getAvailableBitrates().isEmpty());

    // From the features each firmware advertises (CAN_btconst / control_init())
    const uint32_t caps = rig[0].getCapabilities();
    QVERIFY(caps & BusInterface::capability_listen_only);
    QVERIFY(caps & BusInterface::capability_one_shot);
    QVERIFY(!(caps & BusInterface::capability_triple_sampling));
    QCOMPARE(bool(caps & BusInterface::capability_canfd), fw == Fw::ElmueG431);
    QCOMPARE(bool(caps & BusInterface::capability_bus_off_recovery), fw == Fw::CandleLightF072);
    QCOMPARE(bool(caps & BusInterface::capability_tdc), false);

    // Discovery opened the device briefly and closed it again
    QCOMPARE(rig.dev.usbOpenCount(), 0);

    // ...and what DEVICE_CONFIG said is still known (gs_usb.c / Elmue's BCD)
    QCOMPARE(rig[0].getVersion(), fw == Fw::CandleLightF072 ? QStringLiteral("SW 2, HW 1")
                                                            : QStringLiteral("SW 0x261001, HW 0x200"));
}

void CandleDriverTest::skipsDeviceInUse()
{
    FakeGsUsbDevice dev(Fw::ElmueG431);
    dev.setInUse(true);

    QVERIFY(candleDiscoverDevices().empty());
    QCOMPARE(logcapture::errors().size(), 1);
}


// ── open() ───────────────────────────────────────────────────────────────────

void CandleDriverTest::opensWithoutRefusedRequests()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    QVERIFY(rig[0].isOpen());
    QVERIFY(rig.dev.started(0));
    QString stalledReqs;
    for (uint8_t r : rig.dev.stalled())
        stalledReqs += QString::number(r) + ' ';
    QVERIFY2(rig.dev.stalled().isEmpty(), qPrintable("stalled: " + stalledReqs));
    QVERIFY2(rig.dev.ignored().isEmpty(), "a request whose data the firmware ignored");
    QVERIFY(logcapture::errors().isEmpty());

    // Only mode bits the firmware advertises; HW timestamps where offered
    const uint32_t flags = rig.dev.startFlags(0);
    QVERIFY(flags & 0x10);  // GS_CAN_MODE_HW_TIMESTAMP
    QCOMPARE(bool(flags & 0x100), fw == Fw::ElmueG431);  // CAN FD
}

// Every combination the setup dialog offers must be the timing the device
// then runs, by the device's own clock.
void CandleDriverTest::offeredBitratesAreWhatTheDeviceRuns_data()
{
    QTest::addColumn<Fw>("fw");
    QTest::addColumn<unsigned>("bitrate");
    QTest::addColumn<unsigned>("samplePoint");
    QTest::addColumn<unsigned>("fdBitrate");
    QTest::addColumn<unsigned>("fdSamplePoint");

    for (Fw fw : {Fw::CandleLightF072, Fw::ElmueG431})
    {
        Rig rig(fw);
        for (const CanTiming &t : rig[0].getAvailableBitrates())
        {
            // CanTiming leaves the FD sample point unset for classic entries
            const unsigned fdSp = t.getBitrateFD() > 0 ? t.getSamplePointFD() : 0;
            QTest::addRow("%s @ %u/%u %u/%u", fwName(fw), t.getBitrate(), t.getSamplePoint(),
                          t.getBitrateFD(), fdSp)
                << fw << t.getBitrate() << t.getSamplePoint() << t.getBitrateFD() << fdSp;
        }
    }
}

void CandleDriverTest::offeredBitratesAreWhatTheDeviceRuns()
{
    QFETCH(Fw, fw);
    QFETCH(unsigned, bitrate);
    QFETCH(unsigned, samplePoint);
    QFETCH(unsigned, fdBitrate);
    QFETCH(unsigned, fdSamplePoint);

    Rig rig(fw);
    MeasurementInterface mi = rig.config();
    mi.setBitrate(bitrate);
    mi.setSamplePoint(static_cast<int>(samplePoint));
    mi.setCanFD(fdBitrate > 0);
    mi.setFdBitrate(fdBitrate);
    mi.setFdSamplePoint(fdSamplePoint);
    rig.open(0, mi);

    QVERIFY(rig[0].isOpen());
    QVERIFY2(rig.dev.ignored().isEmpty(), "the firmware refused a timing it was sent");
    const auto nominal = rig.dev.nominalTiming(0);
    QVERIFY(nominal);
    const auto [rate, sp] = rateOf(*nominal, rig.dev.clockHz());
    QVERIFY2(std::abs(rate - bitrate) <= bitrate / 1000.0, qPrintable(QString::number(rate)));
    QVERIFY2(std::abs(sp - int(samplePoint)) <= 1, qPrintable(QString::number(sp)));

    if (fdBitrate > 0)
    {
        const auto data = rig.dev.dataTiming(0);
        QVERIFY(data);
        const auto [fdRate, fdSp] = rateOf(*data, rig.dev.clockHz());
        QVERIFY2(std::abs(fdRate - fdBitrate) <= fdBitrate / 1000.0, qPrintable(QString::number(fdRate)));
        QVERIFY2(std::abs(fdSp - int(fdSamplePoint)) <= 1, qPrintable(QString::number(fdSp)));
    }
}

// An earlier session (another tool, or cangaroo killed) left the channel
// running at another bitrate. open() must end up with the configured one.
void CandleDriverTest::deviceLeftRunningIsReconfigured()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    // 250 kbit/s: 48 MHz / (12 * 16), 160 MHz / (4 * 160)
    const FakeGsUsbDevice::Timing old = fw == Fw::CandleLightF072
        ? FakeGsUsbDevice::Timing{.prop = 1, .seg1 = 12, .seg2 = 2, .sjw = 1, .brp = 12}
        : FakeGsUsbDevice::Timing{.prop = 1, .seg1 = 138, .seg2 = 20, .sjw = 1, .brp = 4};
    rig.dev.leaveRunning(0, old);

    rig.open();  // 500 kbit/s
    QVERIFY(rig[0].isOpen());
    const auto nominal = rig.dev.nominalTiming(0);
    QVERIFY(nominal);
    QCOMPARE(qRound(rateOf(*nominal, rig.dev.clockHz()).first), 500000);
    QVERIFY(rig.dev.ignored().isEmpty());
}

void CandleDriverTest::reopensAfterClose()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();
    rig[0].close();
    QVERIFY(!rig.dev.started(0));
    QCOMPARE(rig.dev.usbOpenCount(), 0);

    rig.open();
    QVERIFY(rig[0].isOpen());
    QVERIFY(rig.dev.started(0));
    QVERIFY(rig.dev.ignored().isEmpty());

    rig.dev.receiveBusFrame(0, Frame{.id = 0x42, .dlc = 1, .data = QByteArray(1, '\x07')});
    const QList<BusMessage> msgs = rig.pump();
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs.first().getId(), 0x42u);
}

// After a measurement the BUS Status window keeps polling for a moment, and its
// context menu offers bus-off recovery: neither may reach the closed device,
// whose USB handle is gone (a NULL dereference in WinUSB's control transfer).
void CandleDriverTest::closedChannelDoesNotTouchDevice()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();
    rig[0].sendMessage(toMessage(Frame{.id = 0x123, .dlc = 2, .data = QByteArray("\x01\x02", 2)}));
    (void)rig.pump();
    rig[0].close();
    QCOMPARE(rig.dev.usbOpenCount(), 0);

    QCOMPARE(rig[0].getState(), uint32_t(BusInterface::state_stopped));
    QVERIFY(!rig[0].recoverFromBusOff());
    rig[0].sendMessage(toMessage(Frame{.id = 0x123, .dlc = 0}));
    QCOMPARE(rig[0].getNumTxErrors(), 1);
}

// ── Frames ───────────────────────────────────────────────────────────────────

void CandleDriverTest::transmitsFrames_data()
{
    QTest::addColumn<Fw>("fw");
    QTest::addColumn<int>("caseIndex");
    for (Fw fw : {Fw::CandleLightF072, Fw::ElmueG431})
        for (int i = 0; i < frameCases().size(); ++i)
            if (!frameCases()[i].needsFd || fw == Fw::ElmueG431)
                QTest::addRow("%s / %s", fwName(fw), frameCases()[i].name) << fw << i;
}

void CandleDriverTest::transmitsFrames()
{
    QFETCH(Fw, fw);
    QFETCH(int, caseIndex);
    const Frame &frame = frameCases()[caseIndex].frame;

    Rig rig(fw);
    rig.open();
    rig[0].sendMessage(toMessage(frame));
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(rig.dev.transmitted(0).size(), 1);
    const Frame onBus = rig.dev.transmitted(0).first();
    QCOMPARE(onBus.id, frame.id);
    QCOMPARE(onBus.extended, frame.extended);
    QCOMPARE(onBus.rtr, frame.rtr);
    QCOMPARE(onBus.fd, frame.fd);
    QCOMPARE(onBus.brs, frame.brs);
    QCOMPARE(onBus.dlc, frame.dlc);
    if (!frame.rtr)
        QCOMPARE(onBus.data, frame.data);

    // Shown once the device has echoed it
    QCOMPARE(msgs.size(), 1);
    QVERIFY(!msgs.first().isRX());
    QCOMPARE(msgs.first().getId(), frame.id);
    QCOMPARE(rig[0].getNumTxFrames(), 1);
    QCOMPARE(rig[0].getNumTxErrors(), 0);
}

void CandleDriverTest::receivesFrames_data()
{
    transmitsFrames_data();
}

void CandleDriverTest::receivesFrames()
{
    QFETCH(Fw, fw);
    QFETCH(int, caseIndex);
    const Frame &frame = frameCases()[caseIndex].frame;

    Rig rig(fw);
    rig.open();
    rig.dev.receiveBusFrame(0, frame);
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(msgs.size(), 1);
    const BusMessage &msg = msgs.first();
    QVERIFY(msg.isRX());
    QCOMPARE(msg.getId(), frame.id);
    QCOMPARE(msg.isExtended(), frame.extended);
    QCOMPARE(msg.isRTR(), frame.rtr);
    QCOMPARE(msg.isFD(), frame.fd);
    QCOMPARE(msg.isBRS(), frame.brs);
    QCOMPARE(msg.getLength(), toMessage(frame).getLength());
    if (!frame.rtr)
        QCOMPARE(payload(msg), frame.data);
    QCOMPARE(rig[0].getNumRxFrames(), 1);
}

// Elmue drops a frame in bus monitoring mode without an echo: it must never
// show up as sent (and must not reach the bus).
void CandleDriverTest::elmueListenOnlyNeverSends()
{
    Rig rig(Fw::ElmueG431);
    MeasurementInterface mi = rig.config();
    mi.setListenOnlyMode(true);
    rig.open(0, mi);
    QVERIFY(rig.dev.listenOnly(0));

    rig[0].sendMessage(toMessage(Frame{.id = 0x123, .dlc = 1, .data = QByteArray(1, '\x01')}));
    const QList<BusMessage> msgs = rig.pump();

    QVERIFY(rig.dev.transmitted(0).isEmpty());
    QVERIFY(msgs.isEmpty());
    QCOMPARE(rig[0].getNumTxFrames(), 0);
}

// GS_CAN_FLAG_OVERFLOW: the device lost frames before this one.
void CandleDriverTest::reportsDeviceOverflow()
{
    Rig rig(Fw::CandleLightF072);
    rig.open();
    rig.dev.overflowNext();
    rig.dev.receiveBusFrame(0, Frame{.id = 0x10, .dlc = 1, .data = QByteArray(1, '\x01')});
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(msgs.size(), 2);
    QVERIFY(msgs[0].errorFlags().testFlag(BusError::Overrun));
    QCOMPARE(msgs[1].getId(), 0x10u);
    QCOMPARE(rig[0].getNumRxOverruns(), 1);
}


// ── Bus state ────────────────────────────────────────────────────────────────

void CandleDriverTest::upstreamReportsErrorWarning()
{
    Rig rig(Fw::CandleLightF072);
    rig.open();

    rig.dev.setErrorCounters(0, 100, 0);  // TX warning
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(msgs.size(), 1);
    QVERIFY(msgs.first().errorFlags().testFlag(BusError::ErrorWarning));
    QCOMPARE(rig[0].getState(), uint32_t(BusInterface::state_warning));
}

// With BUS_OFF_RECOVERY the host takes over: the channel stays bus off until
// it asks (can_check_bus_off_recovery_ok), then the firmware restarts it.
void CandleDriverTest::upstreamBusOffAndHostRecovery()
{
    Rig rig(Fw::CandleLightF072);
    rig.open();  // doAutoRestart off: the driver requests host recovery
    QVERIFY(rig.dev.startFlags(0) & (1u << 18));

    rig.dev.setErrorCounters(0, 256, 0);
    QList<BusMessage> msgs = rig.pump();
    QCOMPARE(msgs.size(), 1);
    QVERIFY(msgs.first().errorFlags().testFlag(BusError::BusOff));
    QCOMPARE(rig[0].getState(), uint32_t(BusInterface::state_bus_off));

    QVERIFY(rig[0].recoverFromBusOff());
    msgs = rig.pump();
    QCOMPARE(msgs.size(), 1);
    QVERIFY(msgs.first().errorFlags().testFlag(BusError::Restarted));
    QCOMPARE(rig[0].getState(), uint32_t(BusInterface::state_ok));
    QVERIFY(rig.dev.ignored().isEmpty());
}

void CandleDriverTest::elmueHasNoHostRecovery()
{
    Rig rig(Fw::ElmueG431);
    rig.open();
    QVERIFY(!rig[0].recoverFromBusOff());
    QVERIFY(rig.dev.stalled().isEmpty());  // it did not even ask
}


// ── Multi channel / lifecycle ────────────────────────────────────────────────

// Multi-channel upstream boards route every channel through one interface;
// gs_host_frame.channel tells them apart.
void CandleDriverTest::twoChannelsShareTheDevice()
{
    Rig rig(Fw::CandleLightF072, 2);
    QCOMPARE(rig.intf.size(), size_t(2));
    rig.open(0);
    rig.open(1);
    QCOMPARE(rig.dev.usbOpenCount(), 1);

    rig.dev.receiveBusFrame(1, Frame{.id = 0x111, .dlc = 1, .data = QByteArray(1, '\x01')});
    rig[0].sendMessage(toMessage(Frame{.id = 0x222, .dlc = 1, .data = QByteArray(1, '\x02')}));

    const QList<BusMessage> on0 = rig.pump(0);
    const QList<BusMessage> on1 = rig.pump(1);
    QCOMPARE(on0.size(), 1);
    QCOMPARE(on0.first().getId(), 0x222u);
    QVERIFY(!on0.first().isRX());
    QCOMPARE(on1.size(), 1);
    QCOMPARE(on1.first().getId(), 0x111u);
    QCOMPARE(rig.dev.transmitted(0).size(), 1);
    QVERIFY(rig.dev.transmitted(1).isEmpty());

    rig[0].close();
    QVERIFY(!rig.dev.started(0));
    QVERIFY(rig.dev.started(1));
    QCOMPARE(rig.dev.usbOpenCount(), 1);  // channel 1 still needs it
    rig[1].close();
    QCOMPARE(rig.dev.usbOpenCount(), 0);
}

// The BusListener calls close() also when open() failed. That channel holds
// no reference: it must neither stop the device nor release the running
// channel's reference (which would close the device under it).
void CandleDriverTest::closeWithoutOpenKeepsOtherChannel()
{
    Rig rig(Fw::CandleLightF072, 2);
    rig[1].close();  // never opened: device closed, no handle
    QCOMPARE(rig.dev.usbOpenCount(), 0);

    rig.open(0);
    rig[1].close();
    QVERIFY(rig.dev.started(0));
    QCOMPARE(rig.dev.usbOpenCount(), 1);

    rig.dev.receiveBusFrame(0, Frame{.id = 0x42, .dlc = 1, .data = QByteArray(1, '\x07')});
    QCOMPARE(rig.pump(0).size(), 1);

    rig[0].close();
    QCOMPARE(rig.dev.usbOpenCount(), 0);
}

// After the adapter is unplugged every bulk IN transfer fails at once; the
// reader thread must not turn that into a busy loop, and the interface must
// go offline instead of waiting forever.
void CandleDriverTest::unplugDoesNotSpin()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    rig.dev.unplug();
    const int before = rig.dev.bulkInCalls();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int calls = rig.dev.bulkInCalls() - before;
    // A loop that pauses 50 ms after a failure: about 6 in 300 ms
    QVERIFY2(calls < 50, qPrintable(QString("%1 bulk IN transfers in 300 ms").arg(calls)));

    rig.pump(0, 1500);
    QVERIFY(!rig[0].isOpen());
    QCOMPARE(logcapture::errors().size(), 1);

    // The listener keeps calling; nothing more is reported or logged.
    QList<BusMessage> msgs;
    QVERIFY(!rig[0].readMessage(msgs, 10));
    QCOMPARE(logcapture::errors().size(), 1);
}

QTEST_GUILESS_MAIN(CandleDriverTest)
#include "CandleDriverTest.moc"
