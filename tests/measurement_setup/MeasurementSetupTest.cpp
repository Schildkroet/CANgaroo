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

// MeasurementSetup::findDbMessage(): how a received frame is mapped to a DBC message.
//
// Exact identifier matching is the baseline. J1939 databases (BA_ "ProtocolType"
// "J1939") additionally match by PGN, because priority and source address are part
// of the 29-bit identifier and vary per frame while the DBC stores one fixed id.

#include <QtTest>

#include "core/BusMessage.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementSetup.h"
#include "db/dbc/DbcParser.h"
#include "db/model/CanDb.h"
#include "db/model/CanDbMessage.h"

namespace
{

// WBSD from the issue #31 / upstream #33 report: PGN 0xFE48, SA 0xFE, prio 3.
constexpr uint32_t k_wbsdDbcId = 0x8CFE48FEu; // bit 31 = extended flag in a DBC
constexpr uint32_t k_eec1DbcId = 0x8CF00400u; // PGN 0xF004

const QString k_j1939Dbc = QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 2365475070 WBSD: 8 ECU_A
 SG_ Value : 0|8@1+ (1,0) [0|255] "" ECU_A

BO_ 2364539904 EEC1: 8 ECU_A
 SG_ Rpm : 0|8@1+ (1,0) [0|255] "" ECU_A

BA_ "ProtocolType" "J1939";
)");

BusMessage extendedFrame(uint32_t id)
{
    BusMessage msg;
    msg.setExtended(true);
    msg.setId(id);
    return msg;
}

BusMessage standardFrame(uint32_t id)
{
    BusMessage msg;
    msg.setExtended(false);
    msg.setId(id);
    return msg;
}

} // namespace

class MeasurementSetupTest : public QObject
{
    Q_OBJECT

private:
    MeasurementSetup _setup{nullptr};

    pCanDb load(const QString &dbc);
    void use(const pCanDb &db);

private slots:
    void init();

    void exactExtendedIdMatches();
    void exactStandardIdMatches();
    void unknownIdIsNotFound();
    void errorFrameIsNeverMatched();
    void linFrameIsNeverMatchedAsCan();

    void j1939MatchesOtherSourceAddress();
    void j1939MatchesOtherPriority();
    void j1939DistinguishesPgns();
    void j1939Pdu1IgnoresDestinationAddress();
    void j1939ExactIdStillMatches();
    void j1939DoesNotMatchStandardFrames();
    void j1939DoesNotMatchStandardDbMessages();
    void j1939LowestIdWinsOnPgnCollision();
    void j1939ExactIdBeatsPgnMatch();

    void plainDatabaseDoesNotMatchByPgn();
    void j1939FlagIsPerDatabase();
    void pgnCacheFollowsDatabaseReload();
    void clearDropsPgnMatches();
};

pCanDb MeasurementSetupTest::load(const QString &dbc)
{
    QTemporaryFile file;
    if (!file.open()) { return {}; }
    QTextStream(&file) << dbc;
    file.flush();
    file.seek(0);

    auto db = pCanDb::create();
    DbcParser parser;
    if (!parser.parseFile(&file, *db)) { return {}; }
    return db;
}

void MeasurementSetupTest::use(const pCanDb &db)
{
    MeasurementNetwork *network = _setup.getNetwork(0) ? _setup.getNetwork(0) : _setup.createNetwork();
    network->addCanDb(db);
    _setup.rebuildMessageCache();
}

void MeasurementSetupTest::init()
{
    _setup.clear();
}

void MeasurementSetupTest::exactExtendedIdMatches()
{
    use(load(k_j1939Dbc));
    CanDbMessage *msg = _setup.findDbMessage(extendedFrame(k_wbsdDbcId & 0x1FFFFFFF));
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("WBSD"));
}

void MeasurementSetupTest::exactStandardIdMatches()
{
    use(load("VERSION \"\"\n\nNS_:\n\nBS_:\n\nBU_: ECU_A\n\nBO_ 291 Std: 8 ECU_A\n SG_ S : 0|8@1+ (1,0) [0|255] \"\" ECU_A\n"));
    CanDbMessage *msg = _setup.findDbMessage(standardFrame(291));
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("Std"));
}

void MeasurementSetupTest::unknownIdIsNotFound()
{
    use(load(k_j1939Dbc));
    QVERIFY(_setup.findDbMessage(standardFrame(0x123)) == nullptr);
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFEFFFEu)) == nullptr);
}

void MeasurementSetupTest::errorFrameIsNeverMatched()
{
    use(load(k_j1939Dbc));
    BusMessage msg = extendedFrame(k_wbsdDbcId & 0x1FFFFFFF);
    msg.setErrorFrame(true);
    QVERIFY(_setup.findDbMessage(msg) == nullptr);
}

void MeasurementSetupTest::linFrameIsNeverMatchedAsCan()
{
    use(load(k_j1939Dbc));
    BusMessage msg = extendedFrame(k_wbsdDbcId & 0x1FFFFFFF);
    msg.setBusType(BusType::LIN);
    QVERIFY(_setup.findDbMessage(msg) == nullptr);
}

void MeasurementSetupTest::j1939MatchesOtherSourceAddress()
{
    use(load(k_j1939Dbc));
    for (const uint32_t sa : {0x00u, 0x11u, 0x33u, 0xFDu}) {
        CanDbMessage *msg = _setup.findDbMessage(extendedFrame(0x0CFE4800u | sa));
        QVERIFY2(msg != nullptr, qPrintable(QString("SA 0x%1").arg(sa, 2, 16)));
        QCOMPARE(msg->getName(), QString("WBSD"));
    }
}

void MeasurementSetupTest::j1939MatchesOtherPriority()
{
    use(load(k_j1939Dbc));
    for (const uint32_t prio : {0u, 3u, 6u, 7u}) {
        CanDbMessage *msg = _setup.findDbMessage(extendedFrame((prio << 26) | 0x00FE48FEu));
        QVERIFY2(msg != nullptr, qPrintable(QString("prio %1").arg(prio)));
        QCOMPARE(msg->getName(), QString("WBSD"));
    }
}

void MeasurementSetupTest::j1939DistinguishesPgns()
{
    use(load(k_j1939Dbc));
    CanDbMessage *wbsd = _setup.findDbMessage(extendedFrame(0x0CFE4811u));
    CanDbMessage *eec1 = _setup.findDbMessage(extendedFrame(0x18F00411u));
    QVERIFY(wbsd && eec1);
    QCOMPARE(wbsd->getName(), QString("WBSD"));
    QCOMPARE(eec1->getName(), QString("EEC1"));

    // Neighbouring PGNs (PDU2: PS is part of the PGN) must not match.
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4911u)) == nullptr);
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4711u)) == nullptr);
}

void MeasurementSetupTest::j1939Pdu1IgnoresDestinationAddress()
{
    // PGN 0xEF00 (proprietary A, PDU1): PS is a destination address.
    use(load(QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 2565809408 ProprietaryA: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BA_ "ProtocolType" "J1939";
)")));
    // 2565809408 = 0x98EF2500 -> extended, PGN 0xEF00, DA 0x25, SA 0x00
    for (const uint32_t da : {0x00u, 0x25u, 0xFFu}) {
        CanDbMessage *msg = _setup.findDbMessage(extendedFrame(0x18EF0011u | (da << 8)));
        QVERIFY2(msg != nullptr, qPrintable(QString("DA 0x%1").arg(da, 2, 16)));
        QCOMPARE(msg->getName(), QString("ProprietaryA"));
    }
}

void MeasurementSetupTest::j1939ExactIdStillMatches()
{
    use(load(k_j1939Dbc));
    QVERIFY(_setup.findDbMessage(extendedFrame(k_eec1DbcId & 0x1FFFFFFF)) != nullptr);
}

void MeasurementSetupTest::j1939DoesNotMatchStandardFrames()
{
    // An 11-bit frame whose number equals a PGN-reducible id must stay unmatched.
    use(load(k_j1939Dbc));
    QVERIFY(_setup.findDbMessage(standardFrame(0x4FE)) == nullptr);
    QVERIFY(_setup.findDbMessage(standardFrame(0x004)) == nullptr);
}

void MeasurementSetupTest::j1939DoesNotMatchStandardDbMessages()
{
    // Standard id 0x400 reduces to PGN 4 (PF 0, PS 4). A 29-bit frame with the same
    // PGN must not be decoded by it: only extended DBC messages enter the PGN cache.
    use(load(QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 1024 StdMsg: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BA_ "ProtocolType" "J1939";
)")));
    QVERIFY(_setup.findDbMessage(extendedFrame(0x18000411u)) == nullptr);
    QVERIFY(_setup.findDbMessage(standardFrame(0x400)) != nullptr);
}

void MeasurementSetupTest::j1939LowestIdWinsOnPgnCollision()
{
    // Same PGN 0xFE48 from two source addresses; the lower id is the documented winner.
    use(load(QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 2365475071 FromFF: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BO_ 2365474820 From04: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BA_ "ProtocolType" "J1939";
)")));
    CanDbMessage *msg = _setup.findDbMessage(extendedFrame(0x0CFE4877u));
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("From04"));
}

void MeasurementSetupTest::j1939ExactIdBeatsPgnMatch()
{
    use(load(QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 2365474820 From04: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BO_ 2365475071 FromFF: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A

BA_ "ProtocolType" "J1939";
)")));
    // 0x0CFE48FF is defined explicitly, so it must not fall through to the PGN entry.
    CanDbMessage *msg = _setup.findDbMessage(extendedFrame(0x0CFE48FFu));
    QVERIFY(msg != nullptr);
    QCOMPARE(msg->getName(), QString("FromFF"));
}

void MeasurementSetupTest::plainDatabaseDoesNotMatchByPgn()
{
    QString dbc = k_j1939Dbc;
    dbc.remove(QStringLiteral("BA_ \"ProtocolType\" \"J1939\";"));
    use(load(dbc));

    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE48FEu)) != nullptr); // exact id
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4811u)) == nullptr); // other SA
}

void MeasurementSetupTest::j1939FlagIsPerDatabase()
{
    // The J1939 database must not make a second, plain database match by PGN.
    use(load(k_j1939Dbc));
    use(load(QStringLiteral(R"(VERSION ""

NS_:

BS_:

BU_: ECU_A

BO_ 2364539905 Plain: 8 ECU_A
 SG_ V : 0|8@1+ (1,0) [0|255] "" ECU_A
)")));

    QVERIFY(_setup.findDbMessage(extendedFrame(0x18F00411u)) != nullptr); // J1939 EEC1
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CF00401u)) != nullptr); // exact id of Plain
    QVERIFY(_setup.findDbMessage(extendedFrame(0x18F00422u)) != nullptr);
    QCOMPARE(_setup.findDbMessage(extendedFrame(0x0CF00401u))->getName(), QString("Plain"));
    QCOMPARE(_setup.findDbMessage(extendedFrame(0x18F00422u))->getName(), QString("EEC1"));
}

void MeasurementSetupTest::pgnCacheFollowsDatabaseReload()
{
    // A reload keeps the CanDb object and updates it in place via updateFrom().
    pCanDb db = load(k_j1939Dbc);
    use(db);
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4811u)) != nullptr);

    QString reduced = k_j1939Dbc;
    reduced.remove(QStringLiteral("BA_ \"ProtocolType\" \"J1939\";"));
    pCanDb plain = load(reduced);

    _setup.beginDatabaseReload();
    db->updateFrom(plain.data());
    _setup.endDatabaseReload();

    QVERIFY(!db->isJ1939());
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4811u)) == nullptr);
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE48FEu)) != nullptr);
}

void MeasurementSetupTest::clearDropsPgnMatches()
{
    use(load(k_j1939Dbc));
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4811u)) != nullptr);

    _setup.clear();
    QVERIFY(_setup.findDbMessage(extendedFrame(0x0CFE4811u)) == nullptr);
}

QTEST_APPLESS_MAIN(MeasurementSetupTest)
#include "MeasurementSetupTest.moc"
