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

// SLCANInterface driven end to end against simulated CANable 1, CANable 2,
// WeAct Studio and ElmueSoft CANable 2.5 adapters (FakeSlcanDevice). What a device does with a
// command comes from its firmware sources (see FakeSlcanDevice.h).

#include <QSet>
#include <QtTest>

#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"
#include "driver/SLCANDriver/SLCANInterface.h"

#include "FakeSlcanDevice.h"
#include "support/LogCapture.h"

using Fw = FakeSlcanDevice::Firmware;
using Frame = FakeSlcanDevice::Frame;

Q_DECLARE_METATYPE(FakeSlcanDevice::Firmware)

namespace
{

const char *fwName(Fw fw)
{
    switch (fw)
    {
    case Fw::CANable1: return "CANable 1";
    case Fw::CANable2: return "CANable 2";
    case Fw::WeAct:    return "WeAct";
    case Fw::Elmue:    return "Elmue";
    }
    return "?";
}

const QList<Fw> k_allFirmwares = {Fw::CANable1, Fw::CANable2, Fw::WeAct, Fw::Elmue};

bool hasFd(Fw fw)
{
    return fw != Fw::CANable1;
}

SLCANInterface::Manufacturer manufacturerOf(Fw fw)
{
    switch (fw)
    {
    case Fw::WeAct: return SLCANInterface::Manufacturer::WeActStudio;
    case Fw::Elmue: return SLCANInterface::Manufacturer::Elmue;
    default:        return SLCANInterface::Manufacturer::CANable;
    }
}

// A device and the interface talking to it, as SLCANDriver pairs them by USB
// ID and strings. `as` overrides the manufacturer the driver was told.
struct Rig
{
    explicit Rig(Fw fw) : Rig(fw, manufacturerOf(fw)) {}
    Rig(Fw fw, SLCANInterface::Manufacturer as)
        : dev(fw)
        , intf(nullptr, 0, QStringLiteral("fake0"), hasFd(fw), as, dev.factory())
    {
    }

    MeasurementInterface config() const
    {
        MeasurementInterface mi;
        mi.setBitrate(500000);
        if (hasFd(dev.firmware()))
        {
            mi.setCanFD(true);
            mi.setFdBitrate(2000000);
        }
        return mi;
    }

    void open(const MeasurementInterface &mi)
    {
        intf.applyConfig(mi);
        intf.flushPendingConfig();
        intf.open();
    }

    void open() { open(config()); }

    // Runs the BusListener loop `passes` times
    QList<BusMessage> pump(int passes = 10)
    {
        QList<BusMessage> all;
        for (int i = 0; i < passes; ++i)
        {
            QList<BusMessage> batch;
            intf.readMessage(batch, 1);
            all += batch;
        }
        return all;
    }

    FakeSlcanDevice dev;  // outlives intf
    SLCANInterface intf;
};

QByteArray counting(int n, uint8_t start = 0)
{
    QByteArray out;
    for (int i = 0; i < n; ++i)
        out.append(static_cast<char>(start + i));
    return out;
}

struct FrameCase
{
    const char *name;
    Frame frame;
    bool needsFd;
};

const QList<FrameCase> &frameCases()
{
    static const QList<FrameCase> cases = {
        {"std data", Frame{.id = 0x123, .dlc = 3, .data = QByteArray("\x11\x22\x33", 3)}, false},
        {"std empty", Frame{.id = 0x7FF, .dlc = 0}, false},
        {"ext data", Frame{.id = 0x1ABCDEF0, .extended = true, .dlc = 8, .data = counting(8, 0xA0)}, false},
        {"std remote", Frame{.id = 0x456, .rtr = true, .dlc = 4}, false},
        {"ext remote", Frame{.id = 0x18DAF110, .extended = true, .rtr = true, .dlc = 8}, false},
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
    static constexpr uint8_t dlcToBytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    msg.setLength(dlcToBytes[f.dlc]);
    for (int i = 0; i < f.data.size(); ++i)
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(f.data.at(i)));
    return msg;
}

QByteArray payload(const BusMessage &msg)
{
    return QByteArray(reinterpret_cast<const char *>(msg.getData()), msg.getLength());
}

void addFirmwareRows(QList<Fw> fws = k_allFirmwares)
{
    QTest::addColumn<Fw>("fw");
    for (Fw fw : std::as_const(fws))
        QTest::newRow(fwName(fw)) << fw;
}

} // namespace

class SlcanDriverTest : public QObject
{
    Q_OBJECT

private slots:
    void init() { logcapture::clear(); }

    // open()
    void opensAndConfigures_data() { addFirmwareRows(); }
    void opensAndConfigures();
    void readsFirmwareVersion_data() { addFirmwareRows(); }
    void readsFirmwareVersion();
    void failsWhenPortDoesNotOpen();
    void probeReadsVersionBeforeOpen_data() { addFirmwareRows(); }
    void probeReadsVersionBeforeOpen();
    void probeRecognisesElmue();
    void probeFailsWhenPortDoesNotOpen();
    void offeredBitratesAreWhatTheDeviceRuns_data();
    void offeredBitratesAreWhatTheDeviceRuns();
    void offeredDataBitratesAreWhatTheDeviceRuns_data();
    void offeredDataBitratesAreWhatTheDeviceRuns();
    void weActTakesCustomTiming();
    void listenOnlyKeepsDeviceSilent_data() { addFirmwareRows(); }
    void listenOnlyKeepsDeviceSilent();
    void weActLeftInBinaryModeIsSwitchedBack();
    void deviceLeftRunningStillReportsTx_data() { addFirmwareRows(); }
    void deviceLeftRunningStillReportsTx();

    // frames
    void receivesFrames_data();
    void receivesFrames();
    void transmitsFrames_data();
    void transmitsFrames();
    void weActReportsFailedTransmission();
    void elmueRefusesTxInListenOnly();
    void elmueUsesConfiguredSamplePoints();
    void elmueRecognisedFromVersionReply();
    void refusedSetupCommandIsLogged();
    void weActConfirmsBurstInOrder();
    void rxIsNotHeldBackByTx_data() { addFirmwareRows(); }
    void rxIsNotHeldBackByTx();

    // lifecycle
    void unplugTakesInterfaceOffline_data() { addFirmwareRows(); }
    void unplugTakesInterfaceOffline();
    void closeTakesDeviceOffBus_data() { addFirmwareRows(); }
    void closeTakesDeviceOffBus();
};


// ── open() ───────────────────────────────────────────────────────────────────

void SlcanDriverTest::opensAndConfigures()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    QVERIFY(rig.intf.isOpen());
    QCOMPARE(rig.intf.getState(), uint32_t(BusInterface::state_ok));
    QVERIFY(rig.dev.onBus());
    QCOMPARE(rig.dev.nominalBitrate(), 500000u);
    if (hasFd(fw))
        QCOMPARE(rig.dev.dataBitrate(), 2000000u);
    QVERIFY(!rig.dev.silent());
    QVERIFY2(rig.dev.rejected().isEmpty(), rig.dev.rejected().join(' ').constData());
    QVERIFY(logcapture::errors().isEmpty());
}

void SlcanDriverTest::readsFirmwareVersion()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    const QString version = rig.intf.getVersion();
    switch (fw)
    {
    case Fw::CANable1: QVERIFY(version.startsWith("9fddea4 ")); break;
    case Fw::CANable2: QVERIFY(version.startsWith("b158aa7 ")); break;
    case Fw::WeAct:    QVERIFY(version.startsWith("WeAct Studio")); break;
    case Fw::Elmue:
        // ~200 bytes over several USB packets: all of it, tabs made readable
        QVERIFY(version.startsWith("Board: Multiboard, MCU: STM32G431, "));
        QVERIFY(version.contains("Slcan: 105"));
        QVERIFY(version.endsWith("Serial: 206E38A550565008"));
        break;
    }
    QVERIFY(!version.contains('\r'));
}

// Discovery probes the port so the version is known before a measurement:
// the same reply open() reads, and the CAN channel stays closed.
void SlcanDriverTest::probeReadsVersionBeforeOpen()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    QCOMPARE(rig.intf.getVersion(), QStringLiteral("UnKnown"));

    QVERIFY(rig.intf.probe());
    QVERIFY(!rig.intf.isOpen());
    QVERIFY(!rig.dev.onBus());
    const QString probed = rig.intf.getVersion();
    QVERIFY(!probed.isEmpty() && probed != QStringLiteral("UnKnown"));

    rig.open();
    QVERIFY(rig.intf.isOpen());
    QCOMPARE(rig.intf.getVersion(), probed);
}

// Where the USB strings are missing (Windows) an ElmueSoft board enumerates as
// a CANable 2; the probe must tell it apart before the setup offers options.
void SlcanDriverTest::probeRecognisesElmue()
{
    Rig rig(Fw::Elmue, SLCANInterface::Manufacturer::CANable);
    QVERIFY(!(rig.intf.getCapabilities() & BusInterface::capability_listen_only));

    QVERIFY(rig.intf.probe());
    QVERIFY(!rig.dev.onBus());
    QVERIFY(rig.intf.getCapabilities() & BusInterface::capability_listen_only);
    QVERIFY(rig.intf.getDetailsStr().contains("ElmueSoft"));
    QVERIFY(rig.intf.getVersion().startsWith("Board: Multiboard, MCU: STM32G431, "));
}

void SlcanDriverTest::probeFailsWhenPortDoesNotOpen()
{
    Rig rig(Fw::WeAct);
    rig.dev.setPortOpenFails(true);
    QVERIFY(!rig.intf.probe());
    QCOMPARE(rig.intf.getVersion(), QStringLiteral("UnKnown"));
    QVERIFY(logcapture::errors().isEmpty());
}

void SlcanDriverTest::failsWhenPortDoesNotOpen()
{
    Rig rig(Fw::WeAct);
    rig.dev.setPortOpenFails(true);
    rig.open();

    QVERIFY(!rig.intf.isOpen());
    QCOMPARE(logcapture::errors().size(), 1);
    QVERIFY(rig.dev.commands().isEmpty());

    QList<BusMessage> msgs;
    QVERIFY(!rig.intf.readMessage(msgs, 1));
}

// Every nominal rate offered in the setup dialog must be the rate the device
// then runs at, by the device's own bitrate table.
void SlcanDriverTest::offeredBitratesAreWhatTheDeviceRuns_data()
{
    QTest::addColumn<Fw>("fw");
    QTest::addColumn<unsigned>("bitrate");

    for (Fw fw : k_allFirmwares)
    {
        Rig rig(fw);
        QSet<unsigned> seen;
        for (const CanTiming &t : rig.intf.getAvailableBitrates())
        {
            if (seen.contains(t.getBitrate()))
                continue;
            seen.insert(t.getBitrate());
            QTest::addRow("%s @ %u", fwName(fw), t.getBitrate()) << fw << t.getBitrate();
        }
    }
}

void SlcanDriverTest::offeredBitratesAreWhatTheDeviceRuns()
{
    QFETCH(Fw, fw);
    QFETCH(unsigned, bitrate);

    Rig rig(fw);
    MeasurementInterface mi = rig.config();
    mi.setBitrate(bitrate);
    rig.open(mi);

    // "S7" is 750k on CANable, 800k on WeAct; CANable 1 has no "S9" (83.3k).
    // Elmue's 160 MHz clock cannot make every rate exactly (75k -> 75012).
    QVERIFY2(qAbs(int64_t(rig.dev.nominalBitrate()) - int64_t(bitrate)) <= bitrate / 1000,
             qPrintable(QString::number(rig.dev.nominalBitrate())));
    QVERIFY(rig.dev.rejected().isEmpty());
}

void SlcanDriverTest::offeredDataBitratesAreWhatTheDeviceRuns_data()
{
    QTest::addColumn<Fw>("fw");
    QTest::addColumn<unsigned>("fdBitrate");

    for (Fw fw : {Fw::CANable2, Fw::WeAct, Fw::Elmue})
    {
        Rig rig(fw);
        QSet<unsigned> seen;
        for (const CanTiming &t : rig.intf.getAvailableBitrates())
        {
            if (seen.contains(t.getBitrateFD()))
                continue;
            seen.insert(t.getBitrateFD());
            QTest::addRow("%s @ %u", fwName(fw), t.getBitrateFD()) << fw << t.getBitrateFD();
        }
    }
}

void SlcanDriverTest::offeredDataBitratesAreWhatTheDeviceRuns()
{
    QFETCH(Fw, fw);
    QFETCH(unsigned, fdBitrate);

    Rig rig(fw);
    MeasurementInterface mi = rig.config();
    mi.setFdBitrate(fdBitrate);
    rig.open(mi);

    QCOMPARE(rig.dev.dataBitrate(), fdBitrate);
    QVERIFY(rig.dev.rejected().isEmpty());
}

// The setup page stores custom timings as "ddxxyy" hex (divider, seg1, seg2),
// the WeAct "Sddxxyy" / "Yddxxyy" layout (WeAct README, can_set_bitrate3).
void SlcanDriverTest::weActTakesCustomTiming()
{
    Rig rig(Fw::WeAct);
    MeasurementInterface mi = rig.config();
    mi.setCustomBitrateEn(true);
    mi.setCustomBitrate(0x023407);
    mi.setCustomFdBitrateEn(true);
    mi.setCustomFdBitrate(0x011F08);
    rig.open(mi);

    QVERIFY(rig.dev.rejected().isEmpty());
    QVERIFY(rig.dev.customNominal());
    QCOMPARE(rig.dev.customNominal()->div, uint8_t(0x02));
    QCOMPARE(rig.dev.customNominal()->seg1, uint8_t(0x34));
    QCOMPARE(rig.dev.customNominal()->seg2, uint8_t(0x07));
    QVERIFY(rig.dev.customData());
    QCOMPARE(rig.dev.customData()->div, uint8_t(0x01));
    QCOMPARE(rig.dev.customData()->seg1, uint8_t(0x1F));
    QCOMPARE(rig.dev.customData()->seg2, uint8_t(0x08));
    QVERIFY(rig.dev.onBus());
}

// Listen only must not ACK or drive the bus. An adapter that offers it must
// deliver it; one that cannot (CANable: can_enable() resets the mode to normal)
// must not offer it, and must say so when an older setup still asks for it.
void SlcanDriverTest::listenOnlyKeepsDeviceSilent()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    const bool offered = rig.intf.getCapabilities() & BusInterface::capability_listen_only;
    QCOMPARE(offered, fw == Fw::WeAct || fw == Fw::Elmue);

    MeasurementInterface mi = rig.config();
    mi.setListenOnlyMode(true);
    rig.open(mi);
    QVERIFY(rig.dev.onBus());

    if (offered)
    {
        QVERIFY(rig.dev.silent());
        QVERIFY(logcapture::warnings().isEmpty());
    }
    else
    {
        QVERIFY(!rig.dev.silent());
        QCOMPARE(logcapture::warnings().size(), 1);
    }
}

// Issue #43: another tool left the WeAct in binary (H1) mode, with the channel
// open and traffic on the bus.
void SlcanDriverTest::weActLeftInBinaryModeIsSwitchedBack()
{
    Rig rig(Fw::WeAct);
    rig.dev.leaveRunning(Frame{.id = 0x2AA, .dlc = 2, .data = QByteArray("\x01\x02", 2)}, true);
    rig.open();

    QVERIFY(rig.intf.isOpen());
    QVERIFY(!rig.dev.enhancedMode());
    QCOMPARE(rig.dev.nominalBitrate(), 500000u);
    QVERIFY(rig.intf.getVersion().startsWith("WeAct Studio"));

    const QList<BusMessage> msgs = rig.pump(3);
    QVERIFY(!msgs.isEmpty());
    QCOMPARE(msgs.first().getId(), 0x2AAu);
    QCOMPARE(payload(msgs.first()), QByteArray("\x01\x02", 2));
    QCOMPARE(rig.intf.getNumRxErrors(), 0);
}

// An earlier session left the channel open with traffic on the bus. open()
// tells confirming devices (WeAct) from silent ones (CANable) by whether
// anything answers "C" within 50 ms, so the traffic itself answers it.
void SlcanDriverTest::deviceLeftRunningStillReportsTx()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.dev.leaveRunning(Frame{.id = 0x2AA, .dlc = 1, .data = QByteArray(1, '\x55')});
    rig.open();
    QVERIFY(rig.intf.isOpen());

    const Frame sent{.id = 0x123, .dlc = 1, .data = QByteArray(1, '\x42')};
    rig.intf.sendMessage(toMessage(sent));
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(rig.dev.transmitted().size(), 1);
    const auto tx = std::count_if(msgs.cbegin(), msgs.cend(), [](const BusMessage &m) { return !m.isRX(); });
    // CANable never confirms; the traffic must not make open() think it does.
    QCOMPARE(tx, 1);
}


// ── Frames ───────────────────────────────────────────────────────────────────

void SlcanDriverTest::receivesFrames_data()
{
    QTest::addColumn<Fw>("fw");
    QTest::addColumn<int>("caseIndex");

    for (Fw fw : k_allFirmwares)
    {
        for (int i = 0; i < frameCases().size(); ++i)
        {
            if (frameCases()[i].needsFd && !hasFd(fw))
                continue;
            QTest::addRow("%s / %s", fwName(fw), frameCases()[i].name) << fw << i;
        }
    }
}

void SlcanDriverTest::receivesFrames()
{
    QFETCH(Fw, fw);
    QFETCH(int, caseIndex);
    const Frame &frame = frameCases()[caseIndex].frame;

    Rig rig(fw);
    rig.open();
    rig.dev.receiveBusFrame(frame);
    const QList<BusMessage> msgs = rig.pump(3);

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
    QCOMPARE(rig.intf.getNumRxFrames(), 1);
    QCOMPARE(rig.intf.getNumRxErrors(), 0);
}

void SlcanDriverTest::transmitsFrames_data()
{
    receivesFrames_data();
}

void SlcanDriverTest::transmitsFrames()
{
    QFETCH(Fw, fw);
    QFETCH(int, caseIndex);
    const Frame &frame = frameCases()[caseIndex].frame;

    Rig rig(fw);
    rig.open();
    rig.intf.sendMessage(toMessage(frame));
    const QList<BusMessage> msgs = rig.pump(3);

    // Both firmwares read "b"/"d" as 11-bit and "B"/"D" as 29-bit, and WeAct
    // (since V1.0.0.6) rejects a remote frame line that carries data digits.
    QVERIFY2(rig.dev.rejected().isEmpty(), rig.dev.rejected().join(' ').constData());
    QCOMPARE(rig.dev.transmitted().size(), 1);
    const Frame &onBus = rig.dev.transmitted().first();
    QCOMPARE(onBus.id, frame.id);
    QCOMPARE(onBus.extended, frame.extended);
    QCOMPARE(onBus.rtr, frame.rtr);
    QCOMPARE(onBus.fd, frame.fd);
    QCOMPARE(onBus.brs, frame.brs);
    QCOMPARE(onBus.dlc, frame.dlc);
    if (!frame.rtr)
        QCOMPARE(onBus.data, frame.data);

    // Shown once in the trace, as sent
    QCOMPARE(msgs.size(), 1);
    QVERIFY(!msgs.first().isRX());
    QCOMPARE(msgs.first().getId(), frame.id);
    QCOMPARE(rig.intf.getNumTxFrames(), 1);
}

// WeAct answers a frame with BEL when it fails on the bus (HAL_FDCAN_ErrorCallback)
void SlcanDriverTest::weActReportsFailedTransmission()
{
    Rig rig(Fw::WeAct);
    rig.open();
    rig.dev.failNextTransmissions(1);

    for (uint32_t id : {0x100u, 0x200u, 0x300u})
        rig.intf.sendMessage(toMessage(Frame{.id = id, .dlc = 1, .data = QByteArray(1, '\x01')}));
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(rig.dev.transmitted().size(), 2);
    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs[0].getId(), 0x200u);
    QCOMPARE(msgs[1].getId(), 0x300u);
    QCOMPARE(rig.intf.getNumTxErrors(), 1);
    QCOMPARE(rig.intf.getNumTxFrames(), 2);
}

void SlcanDriverTest::weActConfirmsBurstInOrder()
{
    Rig rig(Fw::WeAct);
    rig.open();

    constexpr int k_frames = 100;
    for (int i = 0; i < k_frames; ++i)
        rig.intf.sendMessage(toMessage(Frame{.id = static_cast<uint32_t>(i), .dlc = 2, .data = counting(2, i)}));
    const QList<BusMessage> msgs = rig.pump();

    QCOMPARE(rig.dev.transmitted().size(), k_frames);
    QCOMPARE(msgs.size(), k_frames);
    for (int i = 0; i < k_frames; ++i)
    {
        QCOMPARE(rig.dev.transmitted()[i].id, uint32_t(i));
        QCOMPARE(msgs[i].getId(), uint32_t(i));
        QVERIFY(!msgs[i].isRX());
    }
    QCOMPARE(rig.intf.getNumTxErrors(), 0);
}

// Elmue answers a frame in bus monitoring mode with BEL (FBK_NoTxInSilentMode):
// it must count as failed, not show up as sent.
void SlcanDriverTest::elmueRefusesTxInListenOnly()
{
    Rig rig(Fw::Elmue);
    MeasurementInterface mi = rig.config();
    mi.setListenOnlyMode(true);
    rig.open(mi);
    QVERIFY(rig.dev.silent());

    rig.intf.sendMessage(toMessage(Frame{.id = 0x123, .dlc = 1, .data = QByteArray(1, '\x01')}));
    const QList<BusMessage> msgs = rig.pump();

    QVERIFY(msgs.isEmpty());
    QVERIFY(rig.dev.transmitted().isEmpty());
    QCOMPARE(rig.intf.getNumTxErrors(), 1);
    QCOMPARE(rig.intf.getNumTxFrames(), 0);
}

// Elmue's "S"/"Y" presets all sample at 75 %. On a bus at 87.5 % that breaks
// BRS frames (seen on hardware), so the driver sets the timing itself.
void SlcanDriverTest::elmueUsesConfiguredSamplePoints()
{
    Rig rig(Fw::Elmue);
    MeasurementInterface mi = rig.config();
    mi.setSamplePoint(875);
    mi.setFdSamplePoint(750);
    rig.open(mi);

    QVERIFY(rig.dev.rejected().isEmpty());
    QCOMPARE(rig.dev.nominalBitrate(), 500000u);
    QCOMPARE(rig.dev.samplePoint(false), 875u);
    QCOMPARE(rig.dev.dataBitrate(), 2000000u);
    QCOMPARE(rig.dev.samplePoint(true), 750u);
}

// Where SLCANDriver cannot read the USB strings (Windows), an Elmue board is
// created as a CANable 2; its "V" reply must switch the interface over.
void SlcanDriverTest::elmueRecognisedFromVersionReply()
{
    Rig rig(Fw::Elmue, SLCANInterface::Manufacturer::CANable);
    QVERIFY(!(rig.intf.getCapabilities() & BusInterface::capability_listen_only));

    MeasurementInterface mi = rig.config();
    mi.setListenOnlyMode(true);  // a setup saved while listen only was offered
    rig.open(mi);
    QVERIFY(rig.intf.isOpen());
    QVERIFY(rig.intf.getCapabilities() & BusInterface::capability_listen_only);
    QVERIFY(rig.dev.silent());
    QVERIFY(logcapture::warnings().isEmpty());  // no "cannot run listen only"

    // Confirmations are used: the refused frame is a TX error, not a sent frame.
    rig.intf.sendMessage(toMessage(Frame{.id = 0x7FF, .dlc = 0}));
    const QList<BusMessage> msgs = rig.pump();
    QVERIFY(msgs.isEmpty());
    QCOMPARE(rig.intf.getNumTxErrors(), 1);
}

// A confirming device that refuses a setup command must not do so silently.
void SlcanDriverTest::refusedSetupCommandIsLogged()
{
    Rig rig(Fw::WeAct);
    MeasurementInterface mi = rig.config();
    mi.setCustomBitrateEn(true);
    mi.setCustomBitrate(0x010101);  // seg1 < 2: can_set_bitrate3() refuses it
    rig.open(mi);

    QCOMPARE(rig.dev.rejected().size(), 1);
    QCOMPARE(logcapture::warnings().size(), 1);
    QVERIFY(logcapture::warnings().first().contains("S010101"));
    QVERIFY(rig.intf.isOpen());  // the channel itself opened
}

// A frame that arrives while the driver writes must not wait for the next one.
void SlcanDriverTest::rxIsNotHeldBackByTx()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    rig.dev.receiveBusFrame(Frame{.id = 0x321, .dlc = 1, .data = QByteArray(1, '\x07')});
    rig.intf.sendMessage(toMessage(Frame{.id = 0x123, .dlc = 1, .data = QByteArray(1, '\x01')}));
    const QList<BusMessage> msgs = rig.pump();

    const auto rx = std::count_if(msgs.cbegin(), msgs.cend(), [](const BusMessage &m) { return m.isRX(); });
    // waitForBytesWritten() reads the RX line into the port's buffer, and a
    // CANable sends nothing after a frame that would wake waitForReadyRead().
    QCOMPARE(rx, 1);
}


// ── Lifecycle ────────────────────────────────────────────────────────────────

void SlcanDriverTest::unplugTakesInterfaceOffline()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();

    rig.dev.unplug();
    rig.pump(3);

    // The port stays open and only reports an error; every wait returns at once.
    QVERIFY(!rig.intf.isOpen());
    QCOMPARE(rig.intf.getState(), uint32_t(BusInterface::state_bus_off));
    QCOMPARE(logcapture::errors().size(), 1);

    // The listener keeps calling; it must not report anything or log again.
    QList<BusMessage> msgs;
    QVERIFY(!rig.intf.readMessage(msgs, 1));
    QCOMPARE(logcapture::errors().size(), 1);
}

void SlcanDriverTest::closeTakesDeviceOffBus()
{
    QFETCH(Fw, fw);
    Rig rig(fw);
    rig.open();
    QVERIFY(rig.dev.onBus());

    rig.intf.close();

    QVERIFY(!rig.intf.isOpen());
    QVERIFY(!rig.dev.onBus());
}

QTEST_GUILESS_MAIN(SlcanDriverTest)
#include "SlcanDriverTest.moc"
