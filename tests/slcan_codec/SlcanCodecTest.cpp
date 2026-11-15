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

// SLCAN (Lawicel) ASCII frame decoding.
//
// This is attacker-adjacent code in the sense that matters for a bus tool: the
// bytes come from a device over a serial link and may be truncated, mistimed or
// garbage. The malformed-input cases below are the point of the file -- a short
// line must be rejected, never read past its end.

#include <QtTest>

#include "core/BusMessage.h"
#include "driver/SLCANDriver/SlcanBitTiming.h"
#include "driver/SLCANDriver/SlcanFrameCodec.h"

class SlcanCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void bitTimingMatchesElmuePresets_data();
    void bitTimingMatchesElmuePresets();
    void bitTimingAtCommonSamplePoint();
    void bitTimingRejectsImpossible();
    void decodesStandardDataFrame();
    void decodesExtendedDataFrame();
    void decodesRemoteFrames();
    void remoteFrameWithNonZeroDlcIsAccepted();
    void remoteFrameDataAreaIsZeroed();
    void remoteFrameIgnoresTrailingPayload();
    void decodesZeroLengthFrame();
    void decodesLowercaseHex();
    void standardIdAboveElevenBitsIsMasked();

    void decodesFdFrame();
    void decodesFdWithBitrateSwitch();
    void fdDlcNibbleMapping_data();
    void fdDlcNibbleMapping();

    void rejectsEmptyLine();
    void rejectsUnknownTypeCharacter();
    void rejectsShortLine();
    void rejectsTruncatedPayload();
    void rejectsNonHexIdentifier();
    void rejectsNonHexPayload();
    void rejectsClassicDlcAboveEight();
    void rejectsFdDlcAboveFifteen();

    void dlcNibbleRoundTrip_data();
    void dlcNibbleRoundTrip();
    void hexNibbleEncoding();
};

void SlcanCodecTest::decodesStandardDataFrame()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("t1234DEADBEEF", msg));

    QCOMPARE(msg.getId(), 0x123u);
    QVERIFY(!msg.isExtended());
    QVERIFY(!msg.isRTR());
    QVERIFY(!msg.isFD());
    QVERIFY(!msg.isBRS());
    QCOMPARE(msg.getLength(), uint8_t(4));
    QCOMPARE(msg.getByte(0), uint8_t(0xDE));
    QCOMPARE(msg.getByte(1), uint8_t(0xAD));
    QCOMPARE(msg.getByte(2), uint8_t(0xBE));
    QCOMPARE(msg.getByte(3), uint8_t(0xEF));
}

void SlcanCodecTest::decodesExtendedDataFrame()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("T1FFFFFFF80011223344556677", msg));

    QCOMPARE(msg.getId(), 0x1FFFFFFFu);
    QVERIFY(msg.isExtended());
    QVERIFY(!msg.isRTR());
    QCOMPARE(msg.getLength(), uint8_t(8));
    QCOMPARE(msg.getByte(0), uint8_t(0x00));
    QCOMPARE(msg.getByte(7), uint8_t(0x77));
}

void SlcanCodecTest::decodesRemoteFrames()
{
    // A remote frame with DLC 0 works.
    BusMessage zero;
    QVERIFY(slcan::parseFrameLine("r1230", zero));
    QCOMPARE(zero.getId(), 0x123u);
    QVERIFY(zero.isRTR());
    QVERIFY(!zero.isExtended());
    QCOMPARE(zero.getLength(), uint8_t(0));

    BusMessage ext;
    QVERIFY(slcan::parseFrameLine("R000004000", ext));
    QCOMPARE(ext.getId(), 0x400u);
    QVERIFY(ext.isRTR());
    QVERIFY(ext.isExtended());
    QCOMPARE(ext.getLength(), uint8_t(0));
}

// A remote frame requests data rather than carrying it, so "r1238" is a complete
// SLCAN line: "remote request for 8 bytes from id 0x123". The DLC must be kept as
// the requested length while no payload digits are expected. Earlier revisions
// demanded 16 payload hex digits here and dropped every such frame as an RX error.
void SlcanCodecTest::remoteFrameWithNonZeroDlcIsAccepted()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("r1238", msg));
    QCOMPARE(msg.getId(), 0x123u);
    QVERIFY(msg.isRTR());
    QVERIFY(!msg.isExtended());
    QCOMPARE(msg.getLength(), uint8_t(8));

    BusMessage ext;
    QVERIFY(slcan::parseFrameLine("R1FFFFFFF4", ext));
    QCOMPARE(ext.getId(), 0x1FFFFFFFu);
    QVERIFY(ext.isRTR());
    QVERIFY(ext.isExtended());
    QCOMPARE(ext.getLength(), uint8_t(4));
}

// The requested length is reported, but the data area must not carry anything --
// including leftovers from a previous frame decoded into the same BusMessage.
void SlcanCodecTest::remoteFrameDataAreaIsZeroed()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("t1238DEADBEEFCAFEF00D", msg));
    QCOMPARE(msg.getByte(0), uint8_t(0xDE));

    QVERIFY(slcan::parseFrameLine("r1238", msg));
    QCOMPARE(msg.getLength(), uint8_t(8));
    for (uint8_t i = 0; i < 8; i++)
    {
        QCOMPARE(msg.getByte(i), uint8_t(0));
    }
}

// Trailing payload on a remote frame is surplus, not a reason to reject the line.
void SlcanCodecTest::remoteFrameIgnoresTrailingPayload()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("r1232AABB", msg));
    QVERIFY(msg.isRTR());
    QCOMPARE(msg.getLength(), uint8_t(2));
    QCOMPARE(msg.getByte(0), uint8_t(0));
    QCOMPARE(msg.getByte(1), uint8_t(0));
}

void SlcanCodecTest::decodesZeroLengthFrame()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("t1230", msg));
    QCOMPARE(msg.getId(), 0x123u);
    QCOMPARE(msg.getLength(), uint8_t(0));
}

void SlcanCodecTest::decodesLowercaseHex()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("t7ab2dead", msg));
    QCOMPARE(msg.getId(), 0x7ABu);
    QCOMPARE(msg.getLength(), uint8_t(2));
    QCOMPARE(msg.getByte(0), uint8_t(0xDE));
    QCOMPARE(msg.getByte(1), uint8_t(0xAD));
}

// A standard-format line can spell an id above the 11-bit range. The frame is
// accepted and the id is masked to 11 bits, because BusMessage::setId() first
// infers "extended" from the magnitude and parseFrameLine then clears that flag.
// Documented rather than endorsed: such a line is malformed SLCAN to begin with.
void SlcanCodecTest::standardIdAboveElevenBitsIsMasked()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("tabc2dead", msg));
    QVERIFY(!msg.isExtended());
    QCOMPARE(msg.getId(), 0xABCu & 0x7FFu);
}

void SlcanCodecTest::decodesFdFrame()
{
    // DLC nibble 9 means 12 bytes on an FD frame.
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("d1009000102030405060708090A0B", msg));

    QCOMPARE(msg.getId(), 0x100u);
    QVERIFY(msg.isFD());
    QVERIFY(!msg.isBRS());
    QCOMPARE(msg.getLength(), uint8_t(12));
    QCOMPARE(msg.getByte(0), uint8_t(0x00));
    QCOMPARE(msg.getByte(11), uint8_t(0x0B));
}

void SlcanCodecTest::decodesFdWithBitrateSwitch()
{
    BusMessage msg;
    QVERIFY(slcan::parseFrameLine("b2001AA", msg));

    QCOMPARE(msg.getId(), 0x200u);
    QVERIFY(msg.isFD());
    QVERIFY(msg.isBRS());
    QCOMPARE(msg.getLength(), uint8_t(1));
    QCOMPARE(msg.getByte(0), uint8_t(0xAA));
}

void SlcanCodecTest::fdDlcNibbleMapping_data()
{
    QTest::addColumn<char>("nibble");
    QTest::addColumn<int>("byteCount");

    QTest::newRow("0")  << '0' << 0;
    QTest::newRow("8")  << '8' << 8;
    QTest::newRow("9")  << '9' << 12;
    QTest::newRow("A") << 'A' << 16;
    QTest::newRow("B") << 'B' << 20;
    QTest::newRow("C") << 'C' << 24;
    QTest::newRow("D") << 'D' << 32;
    QTest::newRow("E") << 'E' << 48;
    QTest::newRow("F") << 'F' << 64;
}

// The CAN FD DLC nibble is a code, not a byte count: 9..F mean 12..64 bytes.
void SlcanCodecTest::fdDlcNibbleMapping()
{
    QFETCH(char, nibble);
    QFETCH(int, byteCount);

    QByteArray line = QByteArray("d100") + nibble;
    line.append(QByteArray(byteCount * 2, '5'));   // 0x55 payload bytes

    BusMessage msg;
    QVERIFY(slcan::parseFrameLine(line, msg));
    QCOMPARE(msg.getLength(), static_cast<uint8_t>(byteCount));
    if (byteCount > 0)
    {
        QCOMPARE(msg.getByte(static_cast<uint8_t>(byteCount - 1)), uint8_t(0x55));
    }
}

void SlcanCodecTest::rejectsEmptyLine()
{
    BusMessage msg;
    QVERIFY(!slcan::parseFrameLine(QByteArray(), msg));
}

void SlcanCodecTest::rejectsUnknownTypeCharacter()
{
    BusMessage msg;
    for (const char *line : { "x1238", "z1238", "S6", "F", "1238" })
    {
        QVERIFY2(!slcan::parseFrameLine(line, msg), line);
    }
}

void SlcanCodecTest::rejectsShortLine()
{
    BusMessage msg;
    // Missing DLC, missing id digits, id shorter than an extended id needs.
    for (const char *line : { "t", "t1", "t12", "t123", "T1FFFFFF" })
    {
        QVERIFY2(!slcan::parseFrameLine(line, msg), line);
    }
}

// The important one: a data frame's DLC promises more payload than the line
// carries. (Remote frames are exempt -- they legitimately carry none.)
void SlcanCodecTest::rejectsTruncatedPayload()
{
    BusMessage msg;
    QVERIFY(!slcan::parseFrameLine("t1238DEADBEEF", msg));        // claims 8, has 4
    QVERIFY(!slcan::parseFrameLine("t1234DEADBE", msg));          // claims 4, has 3.5
    QVERIFY(!slcan::parseFrameLine("d100F0011", msg));            // claims 64, has 2
    QVERIFY(!slcan::parseFrameLine("T1FFFFFFF8AABB", msg));       // extended, claims 8
}

void SlcanCodecTest::rejectsNonHexIdentifier()
{
    BusMessage msg;
    QVERIFY(!slcan::parseFrameLine("tG238DEADBEEF", msg));
    QVERIFY(!slcan::parseFrameLine("t1-38DEADBEEF", msg));
    QVERIFY(!slcan::parseFrameLine("t12 4DEADBEEF", msg));
}

void SlcanCodecTest::rejectsNonHexPayload()
{
    BusMessage msg;
    QVERIFY(!slcan::parseFrameLine("t1232DEZZ", msg));
    QVERIFY(!slcan::parseFrameLine("t1231G0", msg));
}

// A classic CAN frame cannot carry more than 8 bytes, so DLC 9..F is invalid
// there even though it is meaningful for FD.
void SlcanCodecTest::rejectsClassicDlcAboveEight()
{
    BusMessage msg;
    for (const char *line : { "t1239", "t123A", "t123F" })
    {
        QVERIFY2(!slcan::parseFrameLine(line, msg), line);
    }
}

void SlcanCodecTest::rejectsFdDlcAboveFifteen()
{
    // 'G' is not a hex digit at all, so it cannot encode a DLC.
    BusMessage msg;
    QVERIFY(!slcan::parseFrameLine("d100G", msg));
}

void SlcanCodecTest::dlcNibbleRoundTrip_data()
{
    QTest::addColumn<int>("byteCount");

    for (const int len : { 0, 1, 8, 12, 16, 20, 24, 32, 48, 64 })
    {
        QTest::newRow(qPrintable(QString::number(len))) << len;
    }
}

// bytesToDlcNibble and k_dlcToBytes must be inverses for the valid FD lengths;
// otherwise a frame sent out comes back with a different length.
void SlcanCodecTest::dlcNibbleRoundTrip()
{
    QFETCH(int, byteCount);

    const uint8_t nibble = slcan::bytesToDlcNibble(byteCount);
    QVERIFY(nibble < 16);
    QCOMPARE(int(slcan::k_dlcToBytes[nibble]), byteCount);
}

void SlcanCodecTest::hexNibbleEncoding()
{
    QCOMPARE(slcan::hexNibble(0), '0');
    QCOMPARE(slcan::hexNibble(9), '9');
    QCOMPARE(slcan::hexNibble(10), 'A');
    QCOMPARE(slcan::hexNibble(15), 'F');

    // hexNibble and fromHexNibble must round trip.
    for (uint8_t v = 0; v < 16; v++)
    {
        QCOMPARE(slcan::fromHexNibble(slcan::hexNibble(v)), int(v));
    }

    QCOMPARE(slcan::fromHexNibble('g'), -1);
    QCOMPARE(slcan::fromHexNibble(' '), -1);
    QCOMPARE(slcan::fromHexNibble('\0'), -1);
}

// ── Bit timing (SlcanBitTiming.h) ─────────────────────────────────────────
//
// Expected values are the ElmueSoft firmware's own presets for STM32G4 at a
// 160 MHz CAN clock (Firmware/Slcan/control.c, CAN_NOM_BITTIMING_* and
// CAN_DATA_BITTIMING_*), all at 75 %: the calculation must find the same
// bitrate and sample point, with a prescaler no larger than the preset's.

namespace
{
const slcan::TimingLimits k_g4Nominal{.brpMax = 512, .seg1Max = 256, .seg2Max = 128, .sjwMax = 128};
const slcan::TimingLimits k_g4Data{.brpMax = 32, .seg1Max = 32, .seg2Max = 16, .sjwMax = 16};
} // namespace

void SlcanCodecTest::bitTimingMatchesElmuePresets_data()
{
    QTest::addColumn<bool>("data");
    QTest::addColumn<uint>("bitrate");
    QTest::addColumn<uint>("brp");
    QTest::addColumn<uint>("seg1");
    QTest::addColumn<uint>("seg2");

    QTest::newRow("5k")    << false << 5000u    << 100u << 239u << 80u;
    QTest::newRow("10k")   << false << 10000u   << 50u  << 239u << 80u;
    QTest::newRow("20k")   << false << 20000u   << 25u  << 239u << 80u;
    QTest::newRow("33.3k") << false << 33333u   << 15u  << 239u << 80u;
    QTest::newRow("50k")   << false << 50000u   << 10u  << 239u << 80u;
    QTest::newRow("62.5k") << false << 62500u   << 8u   << 239u << 80u;
    QTest::newRow("83.3k") << false << 83333u   << 6u   << 239u << 80u;
    QTest::newRow("100k")  << false << 100000u  << 5u   << 239u << 80u;
    QTest::newRow("125k")  << false << 125000u  << 4u   << 239u << 80u;
    QTest::newRow("250k")  << false << 250000u  << 2u   << 239u << 80u;
    QTest::newRow("500k")  << false << 500000u  << 1u   << 239u << 80u;
    QTest::newRow("800k")  << false << 800000u  << 1u   << 149u << 50u;
    QTest::newRow("1M")    << false << 1000000u << 1u   << 119u << 40u;
    QTest::newRow("d500k") << true  << 500000u  << 8u   << 29u  << 10u;
    QTest::newRow("d1M")   << true  << 1000000u << 4u   << 29u  << 10u;
    QTest::newRow("d2M")   << true  << 2000000u << 2u   << 29u  << 10u;
    QTest::newRow("d4M")   << true  << 4000000u << 2u   << 14u  << 5u;
    QTest::newRow("d5M")   << true  << 5000000u << 2u   << 11u  << 4u;
}

void SlcanCodecTest::bitTimingMatchesElmuePresets()
{
    QFETCH(bool, data);
    QFETCH(uint, bitrate);
    QFETCH(uint, brp);
    QFETCH(uint, seg1);
    QFETCH(uint, seg2);

    const slcan::BitTiming preset{.brp = brp, .seg1 = seg1, .seg2 = seg2};
    const auto t = slcan::calcBitTiming(160000000, bitrate, 750, data ? k_g4Data : k_g4Nominal);
    QVERIFY(t);
    QCOMPARE(qRound(t->bitrate(160000000)), qRound(preset.bitrate(160000000)));
    QCOMPARE(t->samplePointPermille(), preset.samplePointPermille());
    QVERIFY(t->brp <= preset.brp);
    QVERIFY(t->sjw >= 1 && t->sjw <= t->seg2);
}

// 500k at 87.5 %: what made BRS frames work on a real CANable 2.0 with this
// firmware ("s2,139,20,..."); 1 + 279 + 40 would exceed Seg1's 256.
void SlcanCodecTest::bitTimingAtCommonSamplePoint()
{
    const auto t = slcan::calcBitTiming(160000000, 500000, 875, k_g4Nominal);
    QVERIFY(t);
    QCOMPARE(t->brp, 2u);
    QCOMPARE(t->seg1, 139u);
    QCOMPARE(t->seg2, 20u);
    QCOMPARE(t->samplePointPermille(), 875u);

    const auto d = slcan::calcBitTiming(160000000, 2000000, 800, k_g4Data);
    QVERIFY(d);
    QCOMPARE(qRound(d->bitrate(160000000)), 2000000);
    QCOMPARE(d->samplePointPermille(), 800u);
}

void SlcanCodecTest::bitTimingRejectsImpossible()
{
    QVERIFY(!slcan::calcBitTiming(160000000, 0, 875, k_g4Nominal));
    QVERIFY(!slcan::calcBitTiming(160000000, 500000, 1000, k_g4Nominal));
    QVERIFY(!slcan::calcBitTiming(160000000, 100000000, 875, k_g4Nominal));  // fewer than 3 quanta
    QVERIFY(!slcan::calcBitTiming(160000000, 1000, 875, k_g4Nominal));       // needs BRP > 512
}

QTEST_APPLESS_MAIN(SlcanCodecTest)

#include "SlcanCodecTest.moc"
