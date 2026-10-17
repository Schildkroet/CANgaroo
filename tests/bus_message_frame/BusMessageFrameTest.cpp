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

// Frame-level BusMessage behaviour: payload length, identifier/flag packing and
// payload accessor bounds. These are the invariants every driver relies on when
// it builds a frame, so a change here breaks all of them at once.

#include <QtTest>

#include "core/BusMessage.h"

class BusMessageFrameTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultConstructedIsEmpty();

    void lengthIsStoredVerbatim_data();
    void lengthIsStoredVerbatim();

    void oversizedLengthFallsBackToEight();

    void standardIdRoundTrips();
    void extendedIdRoundTrips();
    void largeIdImpliesExtended();
    void idAndFlagsAreIndependent();
    void rawIdCarriesFlags();

    void j1939PgnIsExtractedFromIdentifier_data();
    void j1939PgnIsExtractedFromIdentifier();

    void setDataSetsLengthAndBytes();
    void byteAccessorsAreBoundsChecked();
    void setDataAtIsBoundsChecked();

    void errorFrameFlagRoundTrips();
    void fdAndBrsFlagsAreSeparateFromId();

    void dataAsciiStringMarksNonPrintableBytes();
    void formatDataBytesMatchesPerInstanceFormatters();
};

void BusMessageFrameTest::defaultConstructedIsEmpty()
{
    const BusMessage msg;

    QCOMPARE(msg.getLength(), uint8_t(0));
    QCOMPARE(msg.getId(), 0u);
    QVERIFY(!msg.isExtended());
    QVERIFY(!msg.isRTR());
    QVERIFY(!msg.isFD());
    QVERIFY(!msg.isBRS());
    QVERIFY(!msg.isErrorFrame());
}

void BusMessageFrameTest::lengthIsStoredVerbatim_data()
{
    QTest::addColumn<int>("length");

    // Classic CAN, then the CAN FD steps, then the boundary.
    for (const int len : { 0, 1, 8, 12, 16, 20, 24, 32, 48, 63, 64 })
    {
        QTest::newRow(qPrintable(QString::number(len))) << len;
    }
}

// setLength stores a byte count, not a DLC code: 12 means 12 bytes, not DLC 9.
void BusMessageFrameTest::lengthIsStoredVerbatim()
{
    QFETCH(int, length);

    BusMessage msg;
    msg.setLength(static_cast<uint8_t>(length));

    QCOMPARE(msg.getLength(), static_cast<uint8_t>(length));
}

// Documents a deliberately surprising fallback: a length above the CAN FD
// maximum is coerced to 8, not clamped to 64 and not rejected.
void BusMessageFrameTest::oversizedLengthFallsBackToEight()
{
    for (const int length : { 65, 100, 255 })
    {
        BusMessage msg;
        msg.setLength(static_cast<uint8_t>(length));
        QCOMPARE(msg.getLength(), uint8_t(8));
    }
}

void BusMessageFrameTest::standardIdRoundTrips()
{
    BusMessage msg;
    msg.setId(0x123);

    QCOMPARE(msg.getId(), 0x123u);
    QVERIFY(!msg.isExtended());
}

void BusMessageFrameTest::extendedIdRoundTrips()
{
    BusMessage msg;
    msg.setExtended(true);
    msg.setId(0x1FFFFFFF);

    QCOMPARE(msg.getId(), 0x1FFFFFFFu);
    QVERIFY(msg.isExtended());
}

// Any id beyond the 11-bit range implies an extended frame.
void BusMessageFrameTest::largeIdImpliesExtended()
{
    BusMessage msg;
    msg.setId(0x800);

    QVERIFY(msg.isExtended());
    QCOMPARE(msg.getId(), 0x800u);

    BusMessage borderline;
    borderline.setId(0x7FF);
    QVERIFY(!borderline.isExtended());
}

// Setting an id must not clear the flags packed into the same raw word, and
// toggling flags must not corrupt the id.
void BusMessageFrameTest::idAndFlagsAreIndependent()
{
    BusMessage msg;
    msg.setExtended(true);
    msg.setRTR(true);
    msg.setId(0x1ABCDEF);

    QCOMPARE(msg.getId(), 0x1ABCDEFu);
    QVERIFY(msg.isExtended());
    QVERIFY(msg.isRTR());

    msg.setRTR(false);
    QCOMPARE(msg.getId(), 0x1ABCDEFu);
    QVERIFY(msg.isExtended());
    QVERIFY(!msg.isRTR());

    msg.setExtended(false);
    QVERIFY(!msg.isExtended());
    // getId() now masks to 11 bits, but the stored value is untouched.
    msg.setExtended(true);
    QCOMPARE(msg.getId(), 0x1ABCDEFu);
}

// getRawId() and setRawId() are deliberately NOT inverses, and several callers
// depend on that: getRawId() masks the flag bits off (MeasurementSetup's message
// cache and GatewayWindow's rules key on it, see the comment at
// MeasurementSetup.cpp:117), while setRawId() stores the word verbatim so that
// feeding it a DBC raw id -- where bit 31 marks an extended frame -- also sets
// the extended flag. Pinned here so neither side gets "tidied up" in isolation.
void BusMessageFrameTest::j1939PgnIsExtractedFromIdentifier_data()
{
    QTest::addColumn<uint32_t>("id");
    QTest::addColumn<uint32_t>("pgn");

    // PGNs from SAE J1939-71/-81; priority and source address vary per frame.
    QTest::newRow("EEC1 prio3 sa0")      << 0x0CF00400u << 0xF004u;
    QTest::newRow("EEC1 prio6 sa0x33")   << 0x18F00433u << 0xF004u;
    QTest::newRow("DM1 prio6 sa0")       << 0x18FECA00u << 0xFECAu;
    QTest::newRow("DM1 prio6 sa0xFE")    << 0x18FECAFEu << 0xFECAu;
    QTest::newRow("WBSD DBC id vs bus")  << 0x0CFE48FEu << 0xFE48u;
    QTest::newRow("WBSD other sa")       << 0x0CFE4811u << 0xFE48u;
    // PDU1 (PF < 240): PS is the destination address, not part of the PGN.
    QTest::newRow("address claim to FF") << 0x18EEFF00u << 0xEE00u;
    QTest::newRow("address claim to 25") << 0x18EE2500u << 0xEE00u;
    QTest::newRow("request to 17")       << 0x18EA1700u << 0xEA00u;
    // Data page bit is part of the PGN.
    QTest::newRow("data page 1")         << 0x19F00400u << 0x1F004u;
}

void BusMessageFrameTest::j1939PgnIsExtractedFromIdentifier()
{
    QFETCH(uint32_t, id);
    QFETCH(uint32_t, pgn);
    QCOMPARE(BusMessage::j1939Pgn(id), pgn);
}

void BusMessageFrameTest::rawIdCarriesFlags()
{
    BusMessage msg;
    msg.setExtended(true);
    msg.setRTR(true);
    msg.setId(0x100);

    // Flags are stripped: the raw id is just the identifier.
    QCOMPARE(msg.getRawId(), 0x100u);
    QCOMPARE(msg.getRawId(), msg.getId());

    // A round trip through getRawId() therefore drops the flags.
    BusMessage restored;
    restored.setRawId(msg.getRawId());
    QCOMPARE(restored.getId(), 0x100u);
    QVERIFY(!restored.isExtended());
    QVERIFY(!restored.isRTR());

    // setRawId() keeps whatever flag bits it is given -- this is how a DBC raw
    // id with bit 31 set turns into an extended frame.
    BusMessage fromDbc;
    fromDbc.setRawId(0x80000000u | 0x1ABCDEFu);
    QVERIFY(fromDbc.isExtended());
    QCOMPARE(fromDbc.getId(), 0x1ABCDEFu);
    QCOMPARE(fromDbc.getRawId(), 0x1ABCDEFu);
}

void BusMessageFrameTest::setDataSetsLengthAndBytes()
{
    BusMessage one;
    one.setData(0xAA);
    QCOMPARE(one.getLength(), uint8_t(1));
    QCOMPARE(one.getByte(0), uint8_t(0xAA));

    BusMessage eight;
    eight.setData(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    QCOMPARE(eight.getLength(), uint8_t(8));
    for (uint8_t i = 0; i < 8; i++)
    {
        QCOMPARE(eight.getByte(i), static_cast<uint8_t>(i + 1));
    }
}

// Out-of-range indices must be ignored rather than corrupting memory.
void BusMessageFrameTest::byteAccessorsAreBoundsChecked()
{
    BusMessage msg;
    msg.setLength(64);
    for (uint8_t i = 0; i < 64; i++) { msg.setByte(i, 0x5A); }

    msg.setByte(64, 0xFF);    // beyond k_maxDataBytes
    msg.setByte(200, 0xFF);

    QCOMPARE(msg.getByte(63), uint8_t(0x5A));
    QCOMPARE(msg.getByte(64), uint8_t(0));   // reads outside the buffer yield 0
    QCOMPARE(msg.getByte(255), uint8_t(0));
}

void BusMessageFrameTest::setDataAtIsBoundsChecked()
{
    BusMessage msg;
    msg.setLength(8);
    msg.setDataAt(0, 0x11);
    msg.setDataAt(63, 0x22);
    msg.setDataAt(64, 0x33);   // ignored
    msg.setDataAt(255, 0x44);  // ignored

    QCOMPARE(msg.getByte(0), uint8_t(0x11));
    QCOMPARE(msg.getByte(63), uint8_t(0x22));
}

void BusMessageFrameTest::errorFrameFlagRoundTrips()
{
    BusMessage msg;
    QVERIFY(!msg.isErrorFrame());

    msg.setErrorFrame(true);
    QVERIFY(msg.isErrorFrame());

    msg.setErrorFrame(false);
    QVERIFY(!msg.isErrorFrame());
    QCOMPARE(msg.errorFlags(), BusErrors());
}

// FD/BRS are stored outside the identifier word, so they must survive id edits.
void BusMessageFrameTest::fdAndBrsFlagsAreSeparateFromId()
{
    BusMessage msg;
    msg.setFD(true);
    msg.setBRS(true);
    msg.setId(0x1FFFFFFF);
    msg.setRTR(true);

    QVERIFY(msg.isFD());
    QVERIFY(msg.isBRS());
    QCOMPARE(msg.getId(), 0x1FFFFFFFu);
}

// Settings "Data display: Hex / ASCII" feature: non-printable bytes must
// render as '.' rather than raw control characters, one token per byte
// (space-separated) so DataColumnDelegate's per-byte coloring still works.
void BusMessageFrameTest::dataAsciiStringMarksNonPrintableBytes()
{
    BusMessage msg;
    msg.setLength(4);
    msg.setByte(0, 'A');
    msg.setByte(1, 0x00);
    msg.setByte(2, '!');
    msg.setByte(3, 0x7F);

    QCOMPARE(msg.getDataAsciiString(), QString("A . ! . "));
}

void BusMessageFrameTest::formatDataBytesMatchesPerInstanceFormatters()
{
    BusMessage msg;
    msg.setLength(3);
    msg.setByte(0, 0x01);
    msg.setByte(1, 'B');
    msg.setByte(2, 0xFF);

    const QByteArray data(reinterpret_cast<const char*>(msg.getData()), msg.getLength());

    QCOMPARE(BusMessage::formatDataBytes(data, false), msg.getDataHexString().trimmed());
    QCOMPARE(BusMessage::formatDataBytes(data, true), msg.getDataAsciiString());
    QCOMPARE(BusMessage::formatDataBytes(QByteArray(), false), QString());
    QCOMPARE(BusMessage::formatDataBytes(QByteArray(), true), QString());
}

QTEST_APPLESS_MAIN(BusMessageFrameTest)

#include "BusMessageFrameTest.moc"
