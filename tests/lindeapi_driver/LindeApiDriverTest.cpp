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

// LindeApiInterface / LindeSharedDevice against a simulated lin_usb device
// (FakeLinUsbDevice, transcribed from the firmware's lin_usb.c) behind
// UsbVendorInterface.

#include <chrono>
#include <memory>
#include <thread>

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/LindeApiDriver/LindeApiInterface.h"

#include "FakeLinUsbDevice.h"
#include "support/LogCapture.h"

using namespace std::chrono_literals;

namespace
{

// Two slaves, two schedule tables. The values the tests expect on the device
// are read off this file, not from the driver.
const char k_ldf[] = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 19.2 kbps;

Nodes {
    Master: Gateway, 5 ms, 0.1 ms ;
    Slaves: Seat, Mirror ;
}

Signals {
    Cmd:      16, 0, Gateway, Seat, Mirror;
    SeatPos:  32, 0, Seat, Gateway;
    MirrorX:  16, 0, Mirror, Gateway;
}

Frames {
    GatewayCmd: 0x10, Gateway, 2 {
        Cmd, 0;
    }
    SeatStatus: 0x20, Seat, 4 {
        SeatPos, 0;
    }
    MirrorStatus: 0x21, Mirror, 2 {
        MirrorX, 0;
    }
}

Node_attributes {
    Seat {
        LIN_protocol = "2.1";
        configured_NAD = 0x0A;
    }
    Mirror {
        LIN_protocol = "2.1";
        configured_NAD = 0x0B;
    }
}

Schedule_tables {
    Normal {
        GatewayCmd delay 10 ms;
        SeatStatus delay 20 ms;
        MirrorStatus delay 15 ms;
    }
    Fast {
        GatewayCmd delay 5 ms;
    }
}
)";

// A bus frame as the engine hands it to lin_usb_report_frame()
lin_usb_host_frame_t busFrame(uint8_t ch, uint8_t pid, QByteArray data, uint8_t flags,
                              uint32_t ts = 0)
{
    lin_usb_host_frame_t f{};
    f.echo_id = LIN_USB_ECHO_ID_RX;
    f.timestamp_ms = ts;
    f.lin_id = pid;
    f.channel = ch;
    f.dlc = static_cast<uint8_t>(data.size());
    f.flags = flags;
    std::memcpy(f.data, data.constData(), data.size());
    return f;
}

constexpr uint8_t k_ok = LIN_USB_FRAME_FLAG_RESPONDED | LIN_USB_FRAME_FLAG_VALID;

QByteArray payload(const BusMessage &m)
{
    return QByteArray(reinterpret_cast<const char *>(m.getData()), m.getLength());
}

struct Rig
{
    explicit Rig(int channels = 1, FakeLinUsbDevice::Config config = FakeLinUsbDevice::Config::reference())
        : dev(config)
    {
        shared = std::make_shared<LindeSharedDevice>();
        for (int ch = 0; ch < channels; ++ch)
            intf.push_back(std::make_unique<LindeApiInterface>(nullptr, shared, static_cast<uint8_t>(ch)));
    }
    ~Rig()
    {
        intf.clear();
        shared.reset();
    }

    LindeApiInterface &ch(int n = 0) { return *intf.at(n); }

    // As the BusListener does
    QList<BusMessage> pump(std::chrono::milliseconds duration, int n = 0)
    {
        QList<BusMessage> all;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < duration.count())
            ch(n).readMessage(all, 10);
        return all;
    }

    FakeLinUsbDevice dev;  // outlives the interfaces holding its handle
    std::shared_ptr<LindeSharedDevice> shared;
    std::vector<std::unique_ptr<LindeApiInterface>> intf;
};

MeasurementInterface linSettings(LinNodeMode mode, const QString &ldf = {})
{
    MeasurementInterface mi;
    mi.setBusType(BusType::LIN);
    mi.setLinBaudRate(19200);
    mi.setLinNodeMode(mode);
    mi.setLinLdfPath(ldf);
    return mi;
}

} // namespace

class LindeApiDriverTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init() { logcapture::clear(); }

    void readsDeviceConfig_data();
    void readsDeviceConfig();
    void configuresBusOnOpen();
    void uploadsMasterSchedule();
    void uploadsSlaveTable();
    void listenOnlyStartsWithoutSchedule();
    void advertisesMonitorFromFeatures();
    void receivesFrames_data();
    void receivesFrames();
    void routesFramesPerChannel();
    void runsSchedule();
    void setsFrameData();
    void countsRejectedFrameData();
    void sendsAckEvenWhenDeviceQueueFull();
    void switchesScheduleTable();
    void ignoresScheduleTableNotUploaded();
    void sleepAndWakeup();
    void sendsDiagnosticRequest();
    void mapsBusState();
    void reportsDeviceDropsOfThisSession();
    void keepsTimestampsAcrossTickWrap();
    void sharesDeviceBetweenChannels();
    void reopensAfterClose();
    void survivesUnplug();

private:
    QTemporaryDir _dir;
    QString _ldfPath;
};

void LindeApiDriverTest::initTestCase()
{
    QVERIFY(_dir.isValid());
    _ldfPath = _dir.filePath("test.ldf");
    QFile f(_ldfPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(k_ldf);
}

void LindeApiDriverTest::readsDeviceConfig_data()
{
    QTest::addColumn<bool>("canilfd");
    QTest::addColumn<int>("tables");
    QTest::addColumn<QString>("version");
    QTest::newRow("reference") << false << 4 << QStringLiteral("0.1.0 (HW 1)");
    QTest::newRow("CANILFD") << true << 6 << QStringLiteral("0.1.0 (HW 2)");
}

void LindeApiDriverTest::readsDeviceConfig()
{
    QFETCH(bool, canilfd);
    QFETCH(int, tables);
    QFETCH(QString, version);

    Rig rig(1, canilfd ? FakeLinUsbDevice::Config::canilfd() : FakeLinUsbDevice::Config::reference());
    QCOMPARE(LindeSharedDevice::enumerateDevices(), 1);
    QVERIFY(rig.shared->open());
    QCOMPARE(int(rig.shared->channelCount), 2);
    QCOMPARE(int(rig.shared->scheduleTables), tables);
    QCOMPARE(int(rig.shared->scheduleEntries), 16);
    QCOMPARE(rig.ch().getVersion(), version);
    rig.shared->close();
    QVERIFY(!rig.dev.claimed());
    // Discovery opens briefly like this: what the device reported stays known
    QCOMPARE(rig.ch().getVersion(), version);
    QVERIFY(rig.ch().getCapabilities() & BusInterface::capability_lin_monitor);
}

void LindeApiDriverTest::configuresBusOnOpen()
{
    Rig rig;
    MeasurementInterface mi = linSettings(LinNodeMode::Master);
    mi.setLinBaudRate(10417);
    mi.setLinProtocolVersion(LinProtocolVersion::V2_0);
    rig.ch().applyConfig(mi);
    rig.ch().open();
    QVERIFY(rig.ch().isOpen());

    const auto c = rig.dev.channel(0);
    QVERIFY(c.config.has_value());
    QCOMPARE(c.config->baudrate, 10417u);
    QCOMPARE(int(c.config->lin_version), int(LIN_USB_VERSION_2_0));
    QCOMPARE(int(c.config->break_length), 13);
    QCOMPARE(int(c.config->flags), int(LIN_USB_FLAG_MASTER));
    QCOMPARE(rig.dev.stalls(), 0);
    QVERIFY(logcapture::errors().isEmpty());
}

void LindeApiDriverTest::uploadsMasterSchedule()
{
    Rig rig;
    MeasurementInterface mi = linSettings(LinNodeMode::Master, _ldfPath);
    mi.linFrameDefaultsRef().insert(0x10, QByteArray("\xAB\xCD", 2));
    rig.ch().applyConfig(mi);
    rig.ch().open();
    QVERIFY(rig.ch().isOpen());
    QCOMPARE(rig.dev.stalls(), 0);

    const auto c = rig.dev.channel(0);
    // Normal: the master publishes 0x10, the slaves answer 0x20 / 0x21
    const auto &t0 = c.schedule[0];
    QVERIFY(t0[0] && t0[1] && t0[2] && !t0[3]);
    QCOMPARE(int(t0[0]->lin_id), 0x10);
    QCOMPARE(int(t0[0]->direction), 0);
    QCOMPARE(int(t0[0]->dlc), 2);
    QCOMPARE(int(t0[0]->period_ms), 10);
    QCOMPARE(int(t0[0]->data[0]), 0xAB);
    QCOMPARE(int(t0[0]->data[1]), 0xCD);
    QCOMPARE(int(t0[1]->lin_id), 0x20);
    QCOMPARE(int(t0[1]->direction), 1);
    QCOMPARE(int(t0[1]->dlc), 4);
    QCOMPARE(int(t0[1]->period_ms), 20);
    QCOMPARE(int(t0[2]->lin_id), 0x21);
    QCOMPARE(int(t0[2]->period_ms), 15);
    // Fast
    QVERIFY(c.schedule[1][0] && !c.schedule[1][1]);
    QCOMPARE(int(c.schedule[1][0]->period_ms), 5);

    QVERIFY(c.mode.has_value());
    QCOMPARE(int(c.mode->mode), int(LIN_USB_MODE_START));
    QCOMPARE(int(c.mode->table_id), 0);
    QCOMPARE(int(c.mode->entry_count), 3);
}

// The slave answers its own frames and observes every other one: all frames
// of the LDF in table 0, publishers first, each id once.
void LindeApiDriverTest::uploadsSlaveTable()
{
    Rig rig;
    MeasurementInterface mi = linSettings(LinNodeMode::Slave, _ldfPath);
    mi.setLinSlaveNode(QStringLiteral("Seat"));
    rig.ch().applyConfig(mi);
    rig.ch().open();
    QVERIFY(rig.ch().isOpen());
    QCOMPARE(rig.dev.stalls(), 0);

    const auto c = rig.dev.channel(0);
    QCOMPARE(int(c.config->flags), 0);
    const auto &t0 = c.schedule[0];
    QVERIFY(t0[0] && t0[1] && t0[2] && !t0[3]);
    QCOMPARE(int(t0[0]->lin_id), 0x20);
    QCOMPARE(int(t0[0]->direction), 0);
    QCOMPARE(int(t0[1]->lin_id), 0x10);
    QCOMPARE(int(t0[1]->direction), 1);
    QCOMPARE(int(t0[2]->lin_id), 0x21);
    QCOMPARE(int(t0[2]->direction), 1);
    QCOMPARE(int(c.mode->mode), int(LIN_USB_MODE_START));
    QCOMPARE(int(c.mode->entry_count), 3);
}

void LindeApiDriverTest::listenOnlyStartsWithoutSchedule()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Monitor, _ldfPath));
    rig.ch().open();
    QVERIFY(rig.ch().isOpen());

    const auto c = rig.dev.channel(0);
    QCOMPARE(int(c.config->flags), int(LIN_USB_FLAG_LISTEN_ONLY));
    QVERIFY(!c.schedule[0][0]);
    QCOMPARE(int(c.mode->mode), int(LIN_USB_MODE_START));
    QCOMPARE(int(c.mode->entry_count), 0);
    QVERIFY(logcapture::warnings().isEmpty());
}

void LindeApiDriverTest::advertisesMonitorFromFeatures()
{
    constexpr uint32_t linModes = BusInterface::capability_lin_master | BusInterface::capability_lin_slave |
                                  BusInterface::capability_lin_monitor;

    // Firmware lin_usb.c reports LIN_USB_FEATURE_LISTEN_ONLY
    {
        Rig rig;
        QCOMPARE(rig.ch().getVersion(), QStringLiteral("UnKnown"));
        QCOMPARE(rig.ch().getCapabilities() & linModes, linModes); // not queried yet: assumed
        rig.ch().applyConfig(linSettings(LinNodeMode::Slave));
        rig.ch().open();
        QVERIFY(rig.ch().isOpen());
        QCOMPARE(rig.ch().getCapabilities() & linModes, linModes);
    }

    // Older firmware without it: no monitor once the device has said so,
    // already after the brief open of discovery and across a measurement
    {
        auto config = FakeLinUsbDevice::Config::reference();
        config.features &= ~LIN_USB_FEATURE_LISTEN_ONLY;
        Rig rig(1, config);
        constexpr uint32_t noMonitor = BusInterface::capability_lin_master | BusInterface::capability_lin_slave;

        QVERIFY(rig.shared->open());
        rig.shared->close();
        QCOMPARE(rig.ch().getCapabilities() & linModes, noMonitor);

        rig.ch().applyConfig(linSettings(LinNodeMode::Slave));
        rig.ch().open();
        QVERIFY(rig.ch().isOpen());
        QCOMPARE(rig.ch().getCapabilities() & linModes, noMonitor);
        rig.ch().close();
        QCOMPARE(rig.ch().getCapabilities() & linModes, noMonitor);
    }
}

void LindeApiDriverTest::receivesFrames_data()
{
    QTest::addColumn<uint>("pid");
    QTest::addColumn<uint>("flags");
    QTest::addColumn<uint>("id");
    QTest::addColumn<bool>("rx");
    QTest::addColumn<uint>("error");
    QTest::addColumn<uint>("linFlags");

    // Protected IDs: 0x10 -> 0x50, 0x20 -> 0x20, 0x21 -> 0x61 (P0 = ID0^ID1^ID2^ID4,
    // P1 = !(ID1^ID3^ID4^ID5), LIN 2.2A 2.3.1.3)
    QTest::newRow("published") << 0x50u << uint(k_ok) << 0x10u << false << 0u << 0u;
    QTest::newRow("subscribed")
        << 0x20u << uint(k_ok | LIN_USB_FRAME_FLAG_SUBSCRIBER) << 0x20u << true << 0u << 0u;
    QTest::newRow("no response")
        << 0x61u << uint(LIN_USB_FRAME_FLAG_SUBSCRIBER | LIN_USB_FRAME_FLAG_ERROR) << 0x21u << true
        << uint(BusError::LinNotResponded) << 0u;
    QTest::newRow("checksum")
        << 0x61u
        << uint(LIN_USB_FRAME_FLAG_SUBSCRIBER | LIN_USB_FRAME_FLAG_ERROR | LIN_USB_FRAME_FLAG_RESPONDED)
        << 0x21u << true << uint(BusError::LinChecksumError) << 0u;
    // Sleep / wakeup events as the CANILFD reports them: id 0, no other flags
    QTest::newRow("sleep") << 0x00u << uint(LIN_USB_FRAME_FLAG_SLEEP) << 0x00u << false << 0u
                           << uint(BusMessage::lin_flag_sleep);
    QTest::newRow("wakeup") << 0x00u << uint(LIN_USB_FRAME_FLAG_WAKEUP) << 0x00u << false << 0u
                            << uint(BusMessage::lin_flag_wakeup);
}

void LindeApiDriverTest::receivesFrames()
{
    QFETCH(uint, pid);
    QFETCH(uint, flags);
    QFETCH(uint, id);
    QFETCH(bool, rx);
    QFETCH(uint, error);
    QFETCH(uint, linFlags);

    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master));
    rig.ch().open();
    QVERIFY(rig.ch().isOpen());

    QVERIFY(rig.dev.reportFrame(busFrame(0, uint8_t(pid), QByteArray("\x01\x02", 2), uint8_t(flags))));
    const QList<BusMessage> msgs = rig.pump(100ms);
    QCOMPARE(msgs.size(), 1);
    const BusMessage &m = msgs.first();
    QCOMPARE(m.getId(), id);
    QCOMPARE(m.isRX(), rx);
    QCOMPARE(payload(m), QByteArray("\x01\x02", 2));
    QCOMPARE(uint(m.errorFlags()), error);
    QCOMPARE(uint(m.getFlags()), linFlags);
    QCOMPARE(rx ? rig.ch().getNumRxFrames() : rig.ch().getNumTxFrames(), 1);
}

void LindeApiDriverTest::routesFramesPerChannel()
{
    Rig rig(2);
    for (int n = 0; n < 2; ++n)
    {
        rig.ch(n).applyConfig(linSettings(LinNodeMode::Master));
        rig.ch(n).open();
        QVERIFY(rig.ch(n).isOpen());
    }
    rig.dev.reportFrame(busFrame(1, 0x50, "\x11", k_ok));
    rig.dev.reportFrame(busFrame(0, 0x50, "\x22", k_ok));
    rig.dev.reportFrame(busFrame(1, 0x50, "\x33", k_ok));

    const QList<BusMessage> ch0 = rig.pump(100ms, 0);
    const QList<BusMessage> ch1 = rig.pump(100ms, 1);
    QCOMPARE(ch0.size(), 1);
    QCOMPARE(payload(ch0[0]), QByteArray("\x22"));
    QCOMPARE(ch1.size(), 2);
    QCOMPARE(payload(ch1[0]), QByteArray("\x11"));
    QCOMPARE(payload(ch1[1]), QByteArray("\x33"));
}

// The adapter's master runs its table: Gateway publishes 0x10, nobody answers
// 0x20 / 0x21 here. A set data shows up in the next round.
void LindeApiDriverTest::runsSchedule()
{
    Rig rig;
    MeasurementInterface mi = linSettings(LinNodeMode::Master, _ldfPath);
    mi.linFrameDefaultsRef().insert(0x10, QByteArray("\x01\x02", 2));
    rig.ch().applyConfig(mi);
    rig.ch().open();

    rig.dev.runSchedule(0);
    QList<BusMessage> msgs = rig.pump(100ms);
    QCOMPARE(msgs.size(), 3);
    QCOMPARE(msgs[0].getId(), 0x10u);
    QVERIFY(!msgs[0].isRX());
    QCOMPARE(payload(msgs[0]), QByteArray("\x01\x02", 2));
    QCOMPARE(uint(msgs[0].errorFlags()), 0u);
    QCOMPARE(msgs[1].getId(), 0x20u);
    QVERIFY(msgs[1].isRX());
    QCOMPARE(uint(msgs[1].errorFlags()), uint(BusError::LinNotResponded));
    QCOMPARE(msgs[2].getId(), 0x21u);

    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(0x10);
    msg.setLength(2);
    msg.setDataAt(0, 0x5A);
    msg.setDataAt(1, 0xA5);
    rig.ch().sendMessage(msg);
    rig.dev.runSchedule(0);
    msgs = rig.pump(100ms);
    QCOMPARE(msgs.size(), 3);
    QCOMPARE(payload(msgs[0]), QByteArray("\x5A\xA5", 2));
    QCOMPARE(rig.ch().getNumTxFrames(), 2);
    QCOMPARE(rig.ch().getNumRxFrames(), 4);
    QCOMPARE(rig.ch().getNumRxErrors(), 4);
    QCOMPARE(rig.ch().getNumTxErrors(), 0);
}

// "Sending" updates the payload of the publisher slot; the device ACKs it and
// the ACK is not a bus frame.
void LindeApiDriverTest::setsFrameData()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(0x10);
    msg.setLength(2);
    msg.setDataAt(0, 0x5A);
    msg.setDataAt(1, 0xA5);
    rig.ch().sendMessage(msg);

    const auto c = rig.dev.channel(0);
    QCOMPARE(int(c.setData.size()), 1);
    QCOMPARE(int(c.setData[0].lin_id), 0x10);
    QCOMPARE(int(c.setData[0].dlc), 2);
    QCOMPARE(int(c.setData[0].data[0]), 0x5A);
    QCOMPARE(int(c.setData[0].data[1]), 0xA5);

    QVERIFY(rig.pump(100ms).isEmpty());
    QCOMPARE(rig.ch().getNumTxErrors(), 0);
    QCOMPARE(rig.ch().getNumTxFrames(), 0);  // counted when the device reports it on the bus
}

void LindeApiDriverTest::countsRejectedFrameData()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(0x3A);
    msg.setLength(1);
    rig.ch().sendMessage(msg);

    QVERIFY(rig.pump(100ms).isEmpty());
    QCOMPARE(rig.ch().getNumTxErrors(), 1);
    QCOMPARE(logcapture::warnings().size(), 1);
}

// With the IN queue full the device holds the ACK and NAKs the next packet
// until there is room: nothing may be lost once the host reads again.
void LindeApiDriverTest::sendsAckEvenWhenDeviceQueueFull()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    // A burst while the host is not polling fills the device's queue
    rig.dev.setHostPollsIn(false);
    int accepted = 0;
    for (int i = 0; i < 40; ++i)
        accepted += rig.dev.reportFrame(busFrame(0, 0x50, "\x01", k_ok)) ? 1 : 0;
    QCOMPARE(accepted, 32);

    // First packet: ACK held, OUT endpoint un-armed. The second one is NAKed
    // until the host reads again, 200 ms later: it must wait, not fail.
    std::thread host([&] {
        std::this_thread::sleep_for(200ms);
        rig.dev.setHostPollsIn(true);
    });
    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(0x10);
    msg.setLength(1);
    rig.ch().sendMessage(msg);
    QVERIFY(!rig.dev.outArmed());
    rig.ch().sendMessage(msg);
    host.join();

    const QList<BusMessage> msgs = rig.pump(300ms);
    QCOMPARE(msgs.size(), accepted);
    QVERIFY(logcapture::errors().isEmpty());
    QCOMPARE(int(rig.dev.channel(0).setData.size()), 2);
    QVERIFY(rig.dev.outArmed());  // both ACKs went out
    QCOMPARE(rig.ch().getNumTxErrors(), 0);
}

void LindeApiDriverTest::switchesScheduleTable()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    rig.ch().setLinScheduleTable(1);
    auto c = rig.dev.channel(0);
    QCOMPARE(int(c.mode->mode), int(LIN_USB_MODE_START));
    QCOMPARE(int(c.mode->table_id), 1);
    QCOMPARE(int(c.mode->entry_count), 1);
    QCOMPARE(rig.dev.stalls(), 0);
}

// Selecting a table the LDF does not have must leave the running one alone,
// not stop the master's schedule.
void LindeApiDriverTest::ignoresScheduleTableNotUploaded()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    rig.ch().setLinScheduleTable(3);
    const auto c = rig.dev.channel(0);
    QCOMPARE(int(c.mode->mode), int(LIN_USB_MODE_START));
    QCOMPARE(int(c.mode->table_id), 0);
    QCOMPARE(logcapture::warnings().size(), 1);
}

void LindeApiDriverTest::sleepAndWakeup()
{
    Rig rig(2);
    for (int n = 0; n < 2; ++n)
    {
        rig.ch(n).applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
        rig.ch(n).open();
    }
    rig.ch(1).sendLinSleepWakeup(false);
    QCOMPARE(rig.ch(1).getState(), uint32_t(BusInterface::state_stopped));  // sleeping
    rig.ch(1).sendLinSleepWakeup(true);
    QCOMPARE(rig.ch(1).getState(), uint32_t(BusInterface::state_ok));
    QCOMPARE(rig.dev.channel(1).sleepWakeup, (std::vector<uint8_t>{0, 1}));
    QVERIFY(rig.dev.channel(0).sleepWakeup.empty());

    const QList<BusMessage> msgs = rig.pump(100ms, 1);
    QCOMPARE(msgs.size(), 2);
    QVERIFY(msgs[0].isLinSleepFrame());
    QVERIFY(msgs[1].isLinWakeupFrame());
    QVERIFY(rig.pump(50ms, 0).isEmpty());
}

void LindeApiDriverTest::sendsDiagnosticRequest()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    const uint8_t req[] = {0x02, 0x10, 0x01};  // PCI, SID DiagnosticSessionControl
    rig.ch().sendLinDiagRequest(0x0A, req, sizeof(req));

    const auto c = rig.dev.channel(0);
    QCOMPARE(int(c.setData.size()), 1);
    const auto &f = c.setData[0];
    QCOMPARE(int(f.lin_id), 0x3C);
    QCOMPARE(int(f.dlc), 8);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(f.data), 4), QByteArray("\x0A\x02\x10\x01", 4));
}

void LindeApiDriverTest::mapsBusState()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();

    QCOMPARE(rig.ch().getState(), uint32_t(BusInterface::state_ok));
    rig.dev.setBusState(0, LIN_USB_BUS_STATE_BUS_OFF);
    QCOMPARE(rig.ch().getState(), uint32_t(BusInterface::state_bus_off));
    rig.dev.setBusState(0, LIN_USB_BUS_STATE_ERROR);
    QCOMPARE(rig.ch().getState(), uint32_t(BusInterface::state_warning));
    rig.ch().close();
    QCOMPARE(rig.ch().getState(), uint32_t(BusInterface::state_stopped));
}

// BUS_STATE.dropped counts from the device's last USB reset, not from the
// measurement start: drops of an earlier session must not show up.
void LindeApiDriverTest::reportsDeviceDropsOfThisSession()
{
    Rig rig;
    // The engine kept reporting while no host was reading (32 fit: 31 queued
    // plus the one at the endpoint)
    int dropped = 0;
    for (int i = 0; i < 40; ++i)
        dropped += rig.dev.reportFrame(busFrame(0, 0x50, "\x01", k_ok)) ? 0 : 1;
    QCOMPARE(dropped, 8);

    rig.ch().applyConfig(linSettings(LinNodeMode::Master));
    rig.ch().open();
    rig.pump(100ms);
    rig.ch().getState();
    QCOMPARE(rig.ch().getNumRxOverruns(), 0);
}

// HAL_GetTick() wraps after 49.7 days; frames after the wrap are later, not
// 49 days earlier.
void LindeApiDriverTest::keepsTimestampsAcrossTickWrap()
{
    Rig rig;
    rig.dev.setTickOffset(0xFFFFFF00u);  // 256 ms before the wrap
    rig.ch().applyConfig(linSettings(LinNodeMode::Master));
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    rig.ch().open();

    const uint32_t t = rig.dev.tick();
    rig.dev.reportFrame(busFrame(0, 0x50, "\x01", k_ok, t));
    rig.dev.reportFrame(busFrame(0, 0x50, "\x02", k_ok, t + 1000u));  // past the wrap
    const QList<BusMessage> msgs = rig.pump(100ms);
    QCOMPARE(msgs.size(), 2);
    QVERIFY(msgs[0].getTimestamp_ms() >= before);
    QVERIFY(qAbs(msgs[1].getTimestamp_ms() - msgs[0].getTimestamp_ms() - 1000) <= 2);
}

void LindeApiDriverTest::sharesDeviceBetweenChannels()
{
    Rig rig(2);
    for (int n = 0; n < 2; ++n)
    {
        rig.ch(n).applyConfig(linSettings(LinNodeMode::Master));
        rig.ch(n).open();
    }
    rig.ch(0).close();
    QVERIFY(rig.dev.claimed());
    QCOMPARE(int(rig.dev.channel(0).mode->mode), int(LIN_USB_MODE_STOP));

    rig.dev.reportFrame(busFrame(1, 0x50, "\x01", k_ok));
    QCOMPARE(rig.pump(100ms, 1).size(), 1);

    rig.ch(1).close();
    QVERIFY(!rig.dev.claimed());
}

void LindeApiDriverTest::reopensAfterClose()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master, _ldfPath));
    rig.ch().open();
    rig.ch().close();
    QVERIFY(!rig.dev.claimed());
    QCOMPARE(int(rig.dev.channel(0).mode->mode), int(LIN_USB_MODE_STOP));

    rig.ch().open();
    QVERIFY(rig.ch().isOpen());
    QCOMPARE(int(rig.dev.channel(0).mode->mode), int(LIN_USB_MODE_START));
    rig.dev.reportFrame(busFrame(0, 0x50, "\x01", k_ok));
    QCOMPARE(rig.pump(100ms).size(), 1);
}

void LindeApiDriverTest::survivesUnplug()
{
    Rig rig;
    rig.ch().applyConfig(linSettings(LinNodeMode::Master));
    rig.ch().open();
    rig.dev.unplug();

    QElapsedTimer t;
    t.start();
    QVERIFY(rig.pump(200ms).isEmpty());
    QCOMPARE(rig.ch().getState(), uint32_t(BusInterface::state_unknown));
    QCOMPARE(logcapture::errors().size(), 1);  // once, not per read
    rig.ch().close();
    QVERIFY(!rig.ch().isOpen());
}

QTEST_GUILESS_MAIN(LindeApiDriverTest)
#include "LindeApiDriverTest.moc"
