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

// TraceRecorder: arming, file naming, splitting and the structure of every part.
//
// Structure checks follow the formats themselves (Vector ASC header and footer,
// candump -L line shape, pcapng block framing with interfaces described before
// use) and the input frames. Line and block encoding is covered separately by
// trace_line_format and pcapng_format; tests/format_compat reads recorder output
// back with python-can and scapy.

#include <QtTest>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "core/BusMessage.h"
#include "core/TraceRecorder.h"

namespace
{

// No digit separators in this file: qmake's moc scanner reads ' as a character
// literal, misses Q_OBJECT and generates no TraceRecorderTest.moc.
constexpr qint64 base_us = 1757000000000000LL;

QString interfaceName(BusInterfaceId id)
{
    return QStringLiteral("can%1").arg(id);
}

BusMessage bulkFrame(int i)
{
    BusMessage msg;
    msg.setTimestamp_us(base_us + i * 1000LL);
    msg.setInterfaceId(static_cast<BusInterfaceId>(i % 2));
    msg.setId(static_cast<uint32_t>(0x100 + i % 0x600));
    msg.setRX(true);
    msg.setLength(8);
    for (uint8_t k = 0; k < 8; ++k)
    {
        msg.setByte(k, static_cast<uint8_t>(i + k));
    }
    return msg;
}

RecordingConfig config(const QString &folder, TraceFileFormat format, int splitMb = 0)
{
    return RecordingConfig{
        .folder = folder,
        .fileNamePattern = QStringLiteral("rec"),
        .format = format,
        .splitSizeMb = splitMb,
        .stayArmed = true,
    };
}

QStringList filesIn(const QString &folder)
{
    const QDir dir(folder);
    QStringList files;
    for (const QString &name : dir.entryList(QDir::Files, QDir::Name))
    {
        files << dir.filePath(name);
    }
    return files;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

quint32 le32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

// Frame count of a Vector ASC file, or -1 if header or footer are broken.
int ascFrames(const QByteArray &content)
{
    const QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.size() < 7
        || !lines[0].startsWith(QLatin1String("date "))
        || lines[1] != QLatin1String("base hex  timestamps absolute")
        || !lines[4].startsWith(QLatin1String("Begin Triggerblock "))
        || lines[5].trimmed() != QLatin1String("0.000000 Start of measurement")
        || lines.last() != QLatin1String("End TriggerBlock"))
    {
        return -1;
    }
    return static_cast<int>(lines.size()) - 7;
}

// Frame count of a candump -L file, or -1 if any line has the wrong shape.
int canDumpFrames(const QByteArray &content)
{
    static const QRegularExpression shape(QStringLiteral("^\\(\\d+\\.\\d{6}\\) can[01] [0-9A-F]{3}#[0-9A-F]{16}$"));
    const QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines)
    {
        if (!shape.match(line).hasMatch())
        {
            return -1;
        }
    }
    return static_cast<int>(lines.size());
}

// Enhanced packet count of a pcapng file, or -1 for broken block framing, a
// missing section header, or a packet whose interface was not described first.
int pcapngFrames(const QByteArray &content)
{
    if (content.size() < 12 || le32(content, 0) != 0x0A0D0D0A)
    {
        return -1;
    }
    qsizetype pos = 0;
    quint32 interfaces = 0;
    int packets = 0;
    while (pos < content.size())
    {
        if (pos + 12 > content.size())
        {
            return -1;
        }
        const quint32 type = le32(content, pos);
        const quint32 length = le32(content, pos + 4);
        if (length < 12 || length % 4 != 0 || pos + length > content.size()
            || le32(content, pos + length - 4) != length)
        {
            return -1;
        }
        if (type == 1)
        {
            ++interfaces;
        }
        else if (type == 6)
        {
            if (le32(content, pos + 8) >= interfaces)
            {
                return -1;
            }
            ++packets;
        }
        pos += length;
    }
    return packets;
}

int framesIn(TraceFileFormat format, const QByteArray &content)
{
    switch (format)
    {
        case TraceFileFormat::VectorAsc: return ascFrames(content);
        case TraceFileFormat::PcapNg:    return pcapngFrames(content);
        default:                         return canDumpFrames(content);
    }
}

}

class TraceRecorderTest : public QObject
{
    Q_OBJECT

private slots:
    void fileNameFor_data();
    void fileNameFor();
    void recordsOnlyWhileMeasuring();
    void armingDuringMeasurementStartsImmediately();
    void emptyRecordingIsValid_data();
    void emptyRecordingIsValid();
    void splitsIntoValidParts_data();
    void splitsIntoValidParts();
    void neverOverwrites();
    void stayArmed();
    void unusableFolderDisarms();
    void unsupportedFormatDisarms();
};

void TraceRecorderTest::fileNameFor_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("splitMb");
    QTest::addColumn<int>("index");
    QTest::addColumn<QString>("expected");

    const int asc = static_cast<int>(TraceFileFormat::VectorAsc);
    const int candump = static_cast<int>(TraceFileFormat::CanDump);
    const int pcapng = static_cast<int>(TraceFileFormat::PcapNg);

    QTest::newRow("date and time") << "trace_{date}_{time}" << asc << 0 << 1 << "trace_20260913_140509.asc";
    QTest::newRow("split adds index") << "trace_{date}_{time}" << asc << 100 << 2 << "trace_20260913_140509_002.asc";
    QTest::newRow("explicit index") << "run_{index}" << candump << 100 << 12 << "run_012.candump";
    QTest::newRow("path separators") << "a/b\\c" << pcapng << 0 << 1 << "a_b_c.pcapng";
    QTest::newRow("empty pattern") << "" << asc << 0 << 1 << "trace_20260913_140509.asc";
}

void TraceRecorderTest::fileNameFor()
{
    QFETCH(QString, pattern);
    QFETCH(int, format);
    QFETCH(int, splitMb);
    QFETCH(int, index);
    QFETCH(QString, expected);

    RecordingConfig cfg;
    cfg.fileNamePattern = pattern;
    cfg.format = static_cast<TraceFileFormat>(format);
    cfg.splitSizeMb = splitMb;

    const QDateTime start(QDate(2026, 9, 13), QTime(14, 5, 9));
    QCOMPARE(TraceRecorder::fileNameFor(cfg, start, index), expected);
}

void TraceRecorderTest::recordsOnlyWhileMeasuring()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bool running = false;
    TraceRecorder recorder(interfaceName, [&running]() { return running; });
    recorder.setConfig(config(dir.path(), TraceFileFormat::CanDump));

    recorder.setArmed(true);
    QVERIFY(recorder.isArmed());
    QVERIFY(!recorder.isRecording());
    for (int i = 0; i < 5; ++i)
    {
        recorder.enqueue(bulkFrame(i));  // armed, but no measurement yet
    }

    running = true;
    recorder.onMeasurementStarting();
    QVERIFY(recorder.isRecording());
    for (int i = 5; i < 8; ++i)
    {
        recorder.enqueue(bulkFrame(i));
    }
    recorder.onMeasurementStopped();
    running = false;
    QVERIFY(!recorder.isRecording());

    // Frames after stopping are ignored as well.
    recorder.enqueue(bulkFrame(8));

    QCOMPARE(recorder.framesWritten(), quint64(3));
    const QStringList files = filesIn(dir.path());
    QCOMPARE(files.size(), 1);
    const QByteArray content = readAll(files[0]);
    QCOMPARE(canDumpFrames(content), 3);
    QVERIFY(content.startsWith("(1757000000.005000) can1 105#"));
}

void TraceRecorderTest::armingDuringMeasurementStartsImmediately()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    TraceRecorder recorder(interfaceName, []() { return true; });
    recorder.setConfig(config(dir.path(), TraceFileFormat::CanDump));

    recorder.setArmed(true);
    QVERIFY(recorder.isRecording());
    recorder.enqueue(bulkFrame(0));

    // Disarming stops and finishes the file.
    recorder.setArmed(false);
    QVERIFY(!recorder.isRecording());
    QCOMPARE(recorder.framesWritten(), quint64(1));
    QCOMPARE(canDumpFrames(readAll(filesIn(dir.path()).value(0))), 1);
}

void TraceRecorderTest::emptyRecordingIsValid_data()
{
    QTest::addColumn<int>("format");
    QTest::newRow("asc") << static_cast<int>(TraceFileFormat::VectorAsc);
    QTest::newRow("candump") << static_cast<int>(TraceFileFormat::CanDump);
    QTest::newRow("pcapng") << static_cast<int>(TraceFileFormat::PcapNg);
}

void TraceRecorderTest::emptyRecordingIsValid()
{
    QFETCH(int, format);
    const auto fmt = static_cast<TraceFileFormat>(format);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(config(dir.path(), fmt));
    recorder.setArmed(true);
    recorder.onMeasurementStarting();
    recorder.onMeasurementStopped();

    const QStringList files = filesIn(dir.path());
    QCOMPARE(files.size(), 1);
    QCOMPARE(framesIn(fmt, readAll(files[0])), 0);
}

void TraceRecorderTest::splitsIntoValidParts_data()
{
    QTest::addColumn<int>("format");
    QTest::newRow("asc") << static_cast<int>(TraceFileFormat::VectorAsc);
    QTest::newRow("candump") << static_cast<int>(TraceFileFormat::CanDump);
    QTest::newRow("pcapng") << static_cast<int>(TraceFileFormat::PcapNg);
}

void TraceRecorderTest::splitsIntoValidParts()
{
    QFETCH(int, format);
    const auto fmt = static_cast<TraceFileFormat>(format);
    constexpr int frameCount = 30000;
    constexpr qint64 splitBytes = 1024 * 1024;

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(config(dir.path(), fmt, 1));
    recorder.setArmed(true);
    recorder.onMeasurementStarting();
    for (int i = 0; i < frameCount; ++i)
    {
        recorder.enqueue(bulkFrame(i));
    }
    recorder.onMeasurementStopped();
    QCOMPARE(recorder.framesWritten(), quint64(frameCount));

    const QStringList files = filesIn(dir.path());
    QVERIFY2(files.size() >= 2, qPrintable(QStringLiteral("only %1 part(s)").arg(files.size())));
    QCOMPARE(QFileInfo(files[0]).completeBaseName(), QStringLiteral("rec_001"));

    int total = 0;
    for (const QString &path : files)
    {
        const QByteArray content = readAll(path);
        // A part may overshoot the limit by the one record that crossed it plus the footer.
        QVERIFY2(content.size() <= splitBytes + 256, qPrintable(path));

        const int frames = framesIn(fmt, content);
        QVERIFY2(frames > 0, qPrintable(path));
        total += frames;

        if (fmt == TraceFileFormat::VectorAsc)
        {
            // Every part is a complete trace whose times start at its own first frame.
            const QString firstEvent = QString::fromUtf8(content).split(QLatin1Char('\n')).value(6).trimmed();
            QVERIFY2(firstEvent.startsWith(QLatin1String("0.000000 ")), qPrintable(path));
        }
    }
    QCOMPARE(total, frameCount);
}

void TraceRecorderTest::neverOverwrites()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile existing(dir.filePath(QStringLiteral("rec.candump")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep\n");
    }

    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(config(dir.path(), TraceFileFormat::CanDump));
    recorder.setArmed(true);
    recorder.onMeasurementStarting();
    recorder.enqueue(bulkFrame(0));
    recorder.onMeasurementStopped();

    QCOMPARE(readAll(dir.filePath(QStringLiteral("rec.candump"))), QByteArray("keep\n"));
    QCOMPARE(canDumpFrames(readAll(dir.filePath(QStringLiteral("rec_2.candump")))), 1);
}

void TraceRecorderTest::stayArmed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TraceRecorder recorder(interfaceName, []() { return false; });
    RecordingConfig cfg = config(dir.path(), TraceFileFormat::CanDump);

    cfg.stayArmed = true;
    recorder.setConfig(cfg);
    recorder.setArmed(true);
    recorder.onMeasurementStarting();
    recorder.onMeasurementStopped();
    QVERIFY(recorder.isArmed());

    recorder.onMeasurementStarting();
    QVERIFY(recorder.isRecording());
    cfg.stayArmed = false;
    recorder.setConfig(cfg);
    recorder.onMeasurementStopped();
    QVERIFY(!recorder.isArmed());

    // Each measurement produced its own file.
    QCOMPARE(filesIn(dir.path()).size(), 2);
}

void TraceRecorderTest::unusableFolderDisarms()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile blocker(dir.filePath(QStringLiteral("not_a_folder")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
    }

    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(config(dir.filePath(QStringLiteral("not_a_folder/sub")), TraceFileFormat::CanDump));
    recorder.setArmed(true);
    recorder.onMeasurementStarting();

    QVERIFY(!recorder.isRecording());
    QVERIFY(!recorder.isArmed());
}

void TraceRecorderTest::unsupportedFormatDisarms()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(config(dir.path(), TraceFileFormat::VectorMdf));
    recorder.setArmed(true);
    recorder.onMeasurementStarting();

    QVERIFY(!recorder.isRecording());
    QVERIFY(!recorder.isArmed());
    QVERIFY(filesIn(dir.path()).isEmpty());
}

QTEST_GUILESS_MAIN(TraceRecorderTest)

#include "TraceRecorderTest.moc"
