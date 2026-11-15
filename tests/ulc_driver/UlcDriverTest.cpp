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

// UlcInterface driven end to end against a simulated uLC (FakeUlcDevice).
//
// What the device does with a command, and so every expected table row, comes
// from the firmware sources (see FakeUlcDevice.h), never from UlcFrameCodec:
// the codec is what is being checked here.

#include <memory>

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/UlcDriver/UlcInterface.h"

#include "FakeUlcDevice.h"
#include "support/LogCapture.h"

namespace
{

// Master M publishes 0x10, SlaveA 0x20, SlaveB 0x21.
const char *const k_ldf = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 19.2 kbps;

Nodes {
    Master: M, 5 ms, 0.1 ms ;
    Slaves: SlaveA, SlaveB ;
}

Signals {
    MSig: 32, 0, M, SlaveA, SlaveB;
    ASig: 8, 0, SlaveA, M;
    BSig: 16, 0, SlaveB, M;
}

Frames {
    MasterFrame: 0x10, M, 4 {
        MSig, 0;
    }
    SlaveFrame: 0x20, SlaveA, 8 {
        ASig, 0;
    }
    OtherFrame: 0x21, SlaveB, 2 {
        BSig, 0;
    }
}

Schedule_tables {
    Main {
        MasterFrame delay 10 ms;
        SlaveFrame delay 20 ms;
        OtherFrame delay 30 ms;
    }
    Second {
        MasterFrame delay 5 ms;
    }
}
)";

// Response timeout the firmware gives a row of `len` bytes by itself
// (addLinMasterRow, lin_slcan.c): 1.4 times the frame's bit time, in ms.
constexpr uint8_t firmwareResponseWaitMs(unsigned len, unsigned baud)
{
    return static_cast<uint8_t>(((len * 10u + 44u) * 1400u) / baud + 1u);
}

QByteArray bytes(std::initializer_list<uint8_t> values)
{
    QByteArray out;
    for (uint8_t v : values)
        out.append(static_cast<char>(v));
    return out;
}

BusMessage linFrame(uint8_t id, const QByteArray &data, bool rtr = false)
{
    BusMessage msg;
    msg.setBusType(BusType::LIN);
    msg.setId(id);
    msg.setLength(static_cast<uint8_t>(data.size()));
    for (int i = 0; i < data.size(); ++i)
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(data.at(i)));
    msg.setRTR(rtr);
    return msg;
}

QByteArray payload(const BusMessage &msg)
{
    return QByteArray(reinterpret_cast<const char *>(msg.getData()), msg.getLength());
}

} // namespace

class UlcDriverTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // open()
    void opensAsMonitorWithoutLdf();
    void reportsFirmwareAndHardwareVersion();
    void failsWhenPortDoesNotOpen();
    void failsWhenNoUlcAnswers();
    void setsCustomBaudRate();
    void selectsClassicChecksumForLin13();
    void opensDeviceLeftRunningBySession();
    void listenOnlyOpensMonitor();

    // master
    void masterUploadsScheduleWithLdfTiming();
    void masterUsesFrameDefaults();
    void masterPayloadUpdateReachesSlot();
    void masterPayloadUpdateKeepsSlotTiming();
    void masterRejectsUpdateForSubscribedFrame();
    void masterCoalescesQueuedUpdates();
    void masterSwitchesScheduleTable();

    // slave
    void slaveAnswersOwnFramesAndListensToMaster();

    // monitor
    void monitorReceivesBusFrames();
    void unansweredHeaderIsRxError();
    void monitorOneShotIsShownOnceAsTx();
    void monitorHeaderRequestShowsSlaveAnswer();
    void burstOfFramesNeverOverwritesCommandBuffer();
    void listenOnlyNeverDrivesBus();

    // failures
    void lostReplyTimesOutBeforeNextCommand();
    void unplugTakesInterfaceOffline();
    void closeStopsDevice();

private:
    // Runs the BusListener loop `passes` times, collecting what it reports.
    QList<BusMessage> pump(int passes = 40);
    MeasurementInterface config(LinNodeMode mode) const;
    void openWith(const MeasurementInterface &mi);

    QTemporaryDir _dir;
    QString _ldfPath;
    std::unique_ptr<FakeUlcDevice> _dev;
    std::unique_ptr<UlcInterface> _intf;
};

void UlcDriverTest::initTestCase()
{
    QVERIFY(_dir.isValid());
    _ldfPath = _dir.filePath("test.ldf");
    QFile f(_ldfPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(k_ldf);
}

void UlcDriverTest::init()
{
    logcapture::clear();
    _dev = std::make_unique<FakeUlcDevice>();
    _intf = std::make_unique<UlcInterface>(nullptr, 0, QStringLiteral("fake0"), _dev->factory());
}

void UlcDriverTest::cleanup()
{
    _intf.reset();  // closes while the device still exists
    _dev.reset();
}

QList<BusMessage> UlcDriverTest::pump(int passes)
{
    QList<BusMessage> all;
    for (int i = 0; i < passes; ++i)
    {
        QList<BusMessage> batch;
        _intf->readMessage(batch, 1);
        all += batch;
    }
    return all;
}

MeasurementInterface UlcDriverTest::config(LinNodeMode mode) const
{
    MeasurementInterface mi;
    mi.setBusType(BusType::LIN);
    mi.setLinBaudRate(19200);
    mi.setLinNodeMode(mode);
    if (mode != LinNodeMode::Monitor)
        mi.setLinLdfPath(_ldfPath);
    return mi;
}

void UlcDriverTest::openWith(const MeasurementInterface &mi)
{
    _intf->applyConfig(mi);
    _intf->flushPendingConfig();
    _intf->open();
}


// ── open() ───────────────────────────────────────────────────────────────────

void UlcDriverTest::opensAsMonitorWithoutLdf()
{
    openWith(config(LinNodeMode::Monitor));

    QVERIFY(_intf->isOpen());
    QCOMPARE(_intf->getState(), uint32_t(BusInterface::state_ok));
    QCOMPARE(_dev->state(), FakeUlcDevice::SlcanState::Open);
    QCOMPARE(_dev->linType(), FakeUlcDevice::LinType::Monitor);
    QCOMPARE(_dev->baudRate(), 19200u);
    QVERIFY(_dev->lostCommands().isEmpty());
    QVERIFY(logcapture::errors().isEmpty());
}

void UlcDriverTest::reportsFirmwareAndHardwareVersion()
{
    _dev->setFirmwareVersion(3, 8);
    _dev->setHardwareVersion(1);
    openWith(config(LinNodeMode::Monitor));

    QVERIFY(_intf->getVersion().startsWith("3.8"));
    QVERIFY(_intf->getVersion().contains("01"));
}

void UlcDriverTest::failsWhenPortDoesNotOpen()
{
    _dev->setPortOpenFails(true);
    openWith(config(LinNodeMode::Monitor));

    QVERIFY(!_intf->isOpen());
    QCOMPARE(logcapture::errors().size(), 1);
    QVERIFY(_dev->executedCommands().isEmpty());
}

// Firmware before 3.5 enumerates with ST's generic virtual COM port IDs, so a
// port that never answers "v" must not be taken for a uLC.
void UlcDriverTest::failsWhenNoUlcAnswers()
{
    _dev->setStalled(true);
    openWith(config(LinNodeMode::Monitor));

    QVERIFY(!_intf->isOpen());
    QCOMPARE(logcapture::errors().size(), 1);

    // Must not leave a reader spinning on a dead port
    QList<BusMessage> msgs;
    QVERIFY(!_intf->readMessage(msgs, 1));
}

void UlcDriverTest::setsCustomBaudRate()
{
    MeasurementInterface mi = config(LinNodeMode::Monitor);
    mi.setLinBaudRate(10417);
    openWith(mi);

    QVERIFY(_intf->isOpen());
    QCOMPARE(_dev->baudRate(), 10417u);
    QCOMPARE(_intf->getBitrate(), 10417u);
}

void UlcDriverTest::selectsClassicChecksumForLin13()
{
    MeasurementInterface mi = config(LinNodeMode::Monitor);
    mi.setLinProtocolVersion(LinProtocolVersion::V1_3);
    openWith(mi);
    QVERIFY(_dev->classicChecksum());

    _intf->close();
    mi.setLinProtocolVersion(LinProtocolVersion::V2_1);
    openWith(mi);
    QVERIFY(!_dev->classicChecksum());
}

// An earlier session that was not closed keeps reporting frames, interleaved
// with the replies open() waits for.
void UlcDriverTest::opensDeviceLeftRunningBySession()
{
    _dev->leaveRunningMonitor(0x16, bytes({0x11, 0x22, 0x33}));
    openWith(config(LinNodeMode::Monitor));

    QVERIFY(_intf->isOpen());
    QVERIFY(_intf->getVersion().startsWith("3.8"));
    QCOMPARE(_dev->linType(), FakeUlcDevice::LinType::Monitor);
}

void UlcDriverTest::listenOnlyOpensMonitor()
{
    MeasurementInterface mi = config(LinNodeMode::Master);
    mi.setLinListenOnly(true);
    openWith(mi);

    QVERIFY(_intf->isOpen());
    QCOMPARE(_dev->linType(), FakeUlcDevice::LinType::Monitor);
    QVERIFY(_dev->table().isEmpty());
}


// ── Master ───────────────────────────────────────────────────────────────────

// The firmware sends a row once offset_ms has passed since the previous row's
// header (open_lin_master_dl_handler), so a row carries the LDF delay of the
// entry before it, the first one that of the last (the table wraps).
void UlcDriverTest::masterUploadsScheduleWithLdfTiming()
{
    openWith(config(LinNodeMode::Master));

    QVERIFY(_intf->isOpen());
    QCOMPARE(_dev->linType(), FakeUlcDevice::LinType::Master);
    QCOMPARE(_dev->state(), FakeUlcDevice::SlcanState::Open);  // r1 started the schedule

    const QList<FakeUlcDevice::Row> rows = _dev->table();
    QCOMPARE(rows.size(), 3);

    QCOMPARE(rows[0].pid, uint8_t(0x10));
    QVERIFY(rows[0].transmit);
    QCOMPARE(rows[0].length, uint8_t(4));
    QCOMPARE(rows[0].offsetMs, uint8_t(30));
    QCOMPARE(rows[0].responseWaitMs, firmwareResponseWaitMs(4, 19200));

    QCOMPARE(rows[1].pid, uint8_t(0x20));
    QVERIFY(!rows[1].transmit);
    QCOMPARE(rows[1].length, uint8_t(8));
    QCOMPARE(rows[1].offsetMs, uint8_t(10));
    QCOMPARE(rows[1].responseWaitMs, firmwareResponseWaitMs(8, 19200));

    QCOMPARE(rows[2].pid, uint8_t(0x21));
    QVERIFY(!rows[2].transmit);
    QCOMPARE(rows[2].length, uint8_t(2));
    QCOMPARE(rows[2].offsetMs, uint8_t(20));
    QCOMPARE(rows[2].responseWaitMs, firmwareResponseWaitMs(2, 19200));

    QVERIFY(_dev->lostCommands().isEmpty());
    QVERIFY(logcapture::warnings().isEmpty());
}

void UlcDriverTest::masterUsesFrameDefaults()
{
    MeasurementInterface mi = config(LinNodeMode::Master);
    mi.setLinFrameDefaults({{0x10, bytes({0xDE, 0xAD})}});  // shorter than the frame
    openWith(mi);

    const auto row = _dev->row(0x10);
    QVERIFY(row);
    QCOMPARE(row->data, bytes({0xDE, 0xAD, 0x00, 0x00}));
}

void UlcDriverTest::masterPayloadUpdateReachesSlot()
{
    openWith(config(LinNodeMode::Master));

    _intf->sendMessage(linFrame(0x10, bytes({0xAA, 0xBB, 0xCC, 0xDD})));
    pump();

    const auto row = _dev->row(0x10);
    QVERIFY(row);
    QVERIFY(row->transmit);
    QCOMPARE(row->data, bytes({0xAA, 0xBB, 0xCC, 0xDD}));
    QCOMPARE(_dev->table().size(), 3);  // updated in place, no new row
    QCOMPARE(_intf->getNumTxFrames(), 1);
    QCOMPARE(_intf->getNumTxErrors(), 0);
    QVERIFY(_dev->lostCommands().isEmpty());
}

void UlcDriverTest::masterPayloadUpdateKeepsSlotTiming()
{
    openWith(config(LinNodeMode::Master));
    const auto before = _dev->row(0x10);
    QVERIFY(before);

    _intf->sendMessage(linFrame(0x10, bytes({0xAA, 0xBB, 0xCC, 0xDD})));
    pump();

    const auto after = _dev->row(0x10);
    QVERIFY(after);
    // addLinMasterRow resets offset_ms to 15 and the response timeout to its
    // default for the untimed "t" command, so an update must use the timed form.
    QCOMPARE(after->offsetMs, before->offsetMs);
    QCOMPARE(after->responseWaitMs, before->responseWaitMs);
    QCOMPARE(after->data, bytes({0xAA, 0xBB, 0xCC, 0xDD}));
}

void UlcDriverTest::masterRejectsUpdateForSubscribedFrame()
{
    openWith(config(LinNodeMode::Master));
    const int executed = static_cast<int>(_dev->executedCommands().size());

    // 0x20 is SlaveA's frame: the master only sends its header
    _intf->sendMessage(linFrame(0x20, bytes({1, 2, 3, 4, 5, 6, 7, 8})));
    pump();

    QCOMPARE(_dev->executedCommands().size(), executed);
    QVERIFY(!_dev->row(0x20)->transmit);
    QCOMPARE(_intf->getNumTxErrors(), 1);
    QCOMPARE(logcapture::warnings().size(), 1);
}

void UlcDriverTest::masterCoalescesQueuedUpdates()
{
    openWith(config(LinNodeMode::Master));
    const int executed = static_cast<int>(_dev->executedCommands().size());

    for (uint8_t v = 1; v <= 5; ++v)
        _intf->sendMessage(linFrame(0x10, bytes({v, v, v, v})));
    pump();

    QCOMPARE(_dev->executedCommands().size(), executed + 1);
    QCOMPARE(_dev->row(0x10)->data, bytes({5, 5, 5, 5}));
}

void UlcDriverTest::masterSwitchesScheduleTable()
{
    openWith(config(LinNodeMode::Master));
    _intf->sendMessage(linFrame(0x10, bytes({0xAA, 0xBB, 0xCC, 0xDD})));
    pump();

    _intf->setLinScheduleTable(1);
    pump();

    const QList<FakeUlcDevice::Row> rows = _dev->table();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].pid, uint8_t(0x10));
    QCOMPARE(rows[0].offsetMs, uint8_t(5));  // only slot: its own delay
    QCOMPARE(rows[0].data, bytes({0xAA, 0xBB, 0xCC, 0xDD}));  // payload survives
    QCOMPARE(_dev->state(), FakeUlcDevice::SlcanState::Open);
    QVERIFY(_dev->lostCommands().isEmpty());
}


// ── Slave ────────────────────────────────────────────────────────────────────

void UlcDriverTest::slaveAnswersOwnFramesAndListensToMaster()
{
    MeasurementInterface mi = config(LinNodeMode::Slave);
    mi.setLinSlaveNode("SlaveA");
    openWith(mi);

    QVERIFY(_intf->isOpen());
    QCOMPARE(_dev->linType(), FakeUlcDevice::LinType::Slave);

    // SlaveA publishes 0x20 and listens to the master's 0x10; SlaveB's 0x21
    // must stay out, or the device waits for an answer that may never come.
    const auto own = _dev->row(0x20);
    QVERIFY(own);
    QVERIFY(own->transmit);
    QCOMPARE(own->length, uint8_t(8));

    const auto master = _dev->row(0x10);
    QVERIFY(master);
    QVERIFY(!master->transmit);
    QCOMPARE(master->length, uint8_t(4));

    QVERIFY(!_dev->row(0x21));

    _intf->sendMessage(linFrame(0x20, bytes({8, 7, 6, 5, 4, 3, 2, 1})));
    pump();
    QCOMPARE(_dev->row(0x20)->data, bytes({8, 7, 6, 5, 4, 3, 2, 1}));
}


// ── Monitor ──────────────────────────────────────────────────────────────────

void UlcDriverTest::monitorReceivesBusFrames()
{
    openWith(config(LinNodeMode::Monitor));

    _dev->receiveBusFrame(0x16, bytes({0x11, 0x22, 0x33}));
    _dev->receiveBusFrame(0x3F, bytes({1, 2, 3, 4, 5, 6, 7, 8}));
    const QList<BusMessage> msgs = pump(3);

    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs[0].getId(), 0x16u);
    QVERIFY(msgs[0].isRX());
    QCOMPARE(msgs[0].busType(), BusType::LIN);
    QCOMPARE(payload(msgs[0]), bytes({0x11, 0x22, 0x33}));
    QCOMPARE(msgs[1].getId(), 0x3Fu);
    QCOMPARE(payload(msgs[1]), bytes({1, 2, 3, 4, 5, 6, 7, 8}));
    QCOMPARE(_intf->getNumRxFrames(), 2);
    QCOMPARE(_intf->getNumRxErrors(), 0);
}

void UlcDriverTest::unansweredHeaderIsRxError()
{
    openWith(config(LinNodeMode::Monitor));

    _dev->receiveBusFrame(0x21, {});
    const QList<BusMessage> msgs = pump(3);

    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].getLength(), uint8_t(0));
    QVERIFY(msgs[0].errorFlags().testFlag(BusError::LinNotResponded));
    QCOMPARE(_intf->getNumRxErrors(), 1);
}

// The firmware reports an empty frame for its own one-shot (see transmitStd in
// FakeUlcDevice.cpp); the trace must show the frame once, as sent.
void UlcDriverTest::monitorOneShotIsShownOnceAsTx()
{
    openWith(config(LinNodeMode::Monitor));

    _intf->sendMessage(linFrame(0x12, bytes({0x01, 0x02})));
    const QList<BusMessage> msgs = pump();

    QCOMPARE(_dev->busTransmissions().size(), 1);
    QCOMPARE(_dev->busTransmissions()[0].id, uint8_t(0x12));
    QVERIFY(!_dev->busTransmissions()[0].headerOnly);
    QCOMPARE(_dev->busTransmissions()[0].data, bytes({0x01, 0x02}));

    QCOMPARE(msgs.size(), 1);
    QVERIFY(!msgs[0].isRX());
    QCOMPARE(msgs[0].getId(), 0x12u);
    QCOMPARE(payload(msgs[0]), bytes({0x01, 0x02}));
    QCOMPARE(_intf->getNumTxFrames(), 1);
    QCOMPARE(_intf->getNumRxErrors(), 0);
}

void UlcDriverTest::monitorHeaderRequestShowsSlaveAnswer()
{
    openWith(config(LinNodeMode::Monitor));
    _dev->setSlaveResponse(0x20, bytes({9, 8, 7}));

    _intf->sendMessage(linFrame(0x20, QByteArray(3, '\0'), true));
    const QList<BusMessage> msgs = pump();

    QCOMPARE(_dev->busTransmissions().size(), 1);
    QVERIFY(_dev->busTransmissions()[0].headerOnly);

    QCOMPARE(msgs.size(), 1);
    QVERIFY(msgs[0].isRX());
    QCOMPARE(msgs[0].getId(), 0x20u);
    QCOMPARE(payload(msgs[0]), bytes({9, 8, 7}));
}

// The firmware's single command[] buffer: a command written before the
// previous one was answered replaces it.
void UlcDriverTest::burstOfFramesNeverOverwritesCommandBuffer()
{
    openWith(config(LinNodeMode::Monitor));

    constexpr int k_frames = 20;
    for (int i = 0; i < k_frames; ++i)
        _intf->sendMessage(linFrame(static_cast<uint8_t>(i), bytes({static_cast<uint8_t>(i)})));
    const QList<BusMessage> msgs = pump(3 * k_frames);

    QVERIFY2(_dev->lostCommands().isEmpty(),
             qPrintable("lost: " + QString::fromLatin1(_dev->lostCommands().join(' '))));
    QCOMPARE(_dev->busTransmissions().size(), k_frames);
    for (int i = 0; i < k_frames; ++i)
        QCOMPARE(_dev->busTransmissions()[i].id, uint8_t(i));
    QCOMPARE(msgs.size(), k_frames);
    QCOMPARE(_intf->getNumTxFrames(), k_frames);
}

void UlcDriverTest::listenOnlyNeverDrivesBus()
{
    MeasurementInterface mi = config(LinNodeMode::Monitor);
    mi.setLinListenOnly(true);
    openWith(mi);

    _intf->sendMessage(linFrame(0x12, bytes({1})));
    _intf->sendMessage(linFrame(0x13, bytes({2}), true));
    pump();

    QVERIFY(_dev->busTransmissions().isEmpty());
    QCOMPARE(_intf->getNumTxDropped(), 2);
    QCOMPARE(logcapture::warnings().size(), 1);  // warned once, not per frame
}


// ── Failures ─────────────────────────────────────────────────────────────────

void UlcDriverTest::lostReplyTimesOutBeforeNextCommand()
{
    openWith(config(LinNodeMode::Monitor));

    _dev->dropReplies(1);
    _intf->sendMessage(linFrame(0x01, bytes({1})));
    _intf->sendMessage(linFrame(0x02, bytes({2})));

    pump(20);
    // Still waiting for the first reply: the second must not be written yet
    QCOMPARE(_dev->linesReceived(), static_cast<int>(_dev->executedCommands().size()));
    QCOMPARE(_dev->busTransmissions().size(), 1);

    QElapsedTimer timer;
    timer.start();
    while (_dev->busTransmissions().size() < 2 && timer.elapsed() < 2000)
        pump(5);

    QCOMPARE(_dev->busTransmissions().size(), 2);
    QVERIFY(_dev->lostCommands().isEmpty());
    QCOMPARE(_intf->getNumTxErrors(), 1);  // the unanswered one
    QCOMPARE(_intf->getNumTxFrames(), 1);  // the answered one
}

void UlcDriverTest::unplugTakesInterfaceOffline()
{
    openWith(config(LinNodeMode::Monitor));
    QVERIFY(_intf->isOpen());

    _dev->unplug();
    pump(2);

    QVERIFY(!_intf->isOpen());
    QCOMPARE(logcapture::errors().size(), 1);

    // The listener keeps calling; it must not report anything or log again.
    QList<BusMessage> msgs;
    QVERIFY(!_intf->readMessage(msgs, 1));
    QCOMPARE(logcapture::errors().size(), 1);
}

void UlcDriverTest::closeStopsDevice()
{
    openWith(config(LinNodeMode::Master));
    QCOMPARE(_dev->state(), FakeUlcDevice::SlcanState::Open);

    _intf->close();

    QVERIFY(!_intf->isOpen());
    QCOMPARE(_dev->state(), FakeUlcDevice::SlcanState::Config);
    QCOMPARE(_intf->getState(), uint32_t(BusInterface::state_stopped));
}

QTEST_GUILESS_MAIN(UlcDriverTest)
#include "UlcDriverTest.moc"
