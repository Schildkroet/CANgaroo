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

// Writes a fixed set of frames in every export format, plus TraceRecorder output
// split into parts, so validate.py can read everything back with independent
// readers (python-can, scapy, asammdf).
//
// The frame definitions are duplicated in validate.py on purpose: expectations
// there must not be derived from anything cangaroo writes.

#include <cstdio>
#include <initializer_list>
#include <span>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QVector>

#include "core/BusMessage.h"
#include "core/TraceFileWriter.h"
#include "core/TraceRecorder.h"

namespace
{

constexpr qint64 base_us = 1'757'000'000'000'000LL;
constexpr int bulk_count = 30000;

QString interfaceName(BusInterfaceId id)
{
    return id == 1 ? QStringLiteral("vcan0") : QStringLiteral("vcan1");
}

BusMessage make(qint64 timestampUs, int iface, uint32_t id, bool extended, bool rx, const QByteArray &data)
{
    BusMessage msg;
    msg.setTimestamp_us(timestampUs);
    msg.setInterfaceId(static_cast<BusInterfaceId>(iface));
    msg.setId(id);
    msg.setExtended(extended);
    msg.setRX(rx);
    msg.setLength(static_cast<uint8_t>(data.size()));
    for (int i = 0; i < data.size(); ++i)
    {
        msg.setByte(static_cast<uint8_t>(i), static_cast<uint8_t>(data[i]));
    }
    return msg;
}

// Keep in sync with SAMPLE_FRAMES in validate.py.
QVector<BusMessage> sampleFrames()
{
    QVector<BusMessage> frames;
    frames << make(base_us + 0, 1, 0x123, false, true, QByteArray::fromHex("1122334455667788"));
    frames << make(base_us + 10'000, 1, 0x18DAF110, true, false, QByteArray::fromHex("AABBCC"));

    BusMessage rtr = make(base_us + 20'000, 2, 0x7DF, false, true, QByteArray(4, '\0'));
    rtr.setRTR(true);
    frames << rtr;

    BusMessage fd = make(base_us + 30'000, 2, 0x456, false, true, QByteArray::fromHex("000102030405060708090A0B"));
    fd.setFD(true);
    fd.setBRS(true);
    frames << fd;

    BusMessage fdExtended = make(base_us + 40'000, 1, 0x1ABC0001, true, false, QByteArray(64, static_cast<char>(0xA5)));
    fdExtended.setFD(true);
    frames << fdExtended;

    frames << make(base_us + 50'000, 1, 0x100, false, true, QByteArray());

    BusMessage error = make(base_us + 60'000, 2, 0, false, true, QByteArray());
    error.setErrorFrame(true);
    frames << error;

    return frames;
}

// Keep in sync with bulk_frame() in validate.py.
BusMessage bulkFrame(int i)
{
    QByteArray data;
    for (int k = 0; k < 8; ++k)
    {
        data.append(static_cast<char>((i * 31 + k * 7) & 0xFF));
    }
    return make(base_us + i * 1000LL, 1 + i % 2, static_cast<uint32_t>(0x100 + i % 0x700), false, i % 3 != 0, data);
}

bool record(const QString &folder, TraceFileFormat format)
{
    TraceRecorder recorder(interfaceName, []() { return false; });
    recorder.setConfig(RecordingConfig{
        .folder = folder,
        .fileNamePattern = QStringLiteral("part_{index}"),
        .format = format,
        .splitSizeMb = 1,
        .stayArmed = false,
    });
    recorder.setArmed(true);
    recorder.onMeasurementStarting();
    if (!recorder.isRecording())
    {
        return false;
    }
    for (int i = 0; i < bulk_count; ++i)
    {
        recorder.enqueue(bulkFrame(i));
    }
    recorder.onMeasurementStopped();
    return recorder.framesWritten() == static_cast<quint64>(bulk_count);
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: trace_format_samples <output dir>\n");
        return 2;
    }

    const QDir out(QString::fromLocal8Bit(argv[1]));
    if (!out.mkpath(QStringLiteral(".")))
    {
        std::fprintf(stderr, "cannot create %s\n", argv[1]);
        return 1;
    }

    const QVector<BusMessage> frames = sampleFrames();
    const std::span<const BusMessage> span(frames.constData(), static_cast<std::size_t>(frames.size()));
    const QVector<BusMessage> none;
    const std::span<const BusMessage> empty(none.constData(), 0);

    for (const TraceFileFormat format : { TraceFileFormat::CanDump, TraceFileFormat::VectorAsc,
                                          TraceFileFormat::VectorMdf, TraceFileFormat::Pcap,
                                          TraceFileFormat::PcapNg })
    {
        const QString extension = traceFormatExtension(format);
        QFile file(out.filePath(QStringLiteral("export.") + extension));
        QFile emptyFile(out.filePath(QStringLiteral("empty.") + extension));
        if (!file.open(QIODevice::WriteOnly) || !emptyFile.open(QIODevice::WriteOnly))
        {
            std::fprintf(stderr, "cannot write %s samples\n", qPrintable(extension));
            return 1;
        }
        TraceFileWriter::write(file, format, span, interfaceName);
        TraceFileWriter::write(emptyFile, format, empty, interfaceName);
    }

    for (const TraceFileFormat format : { TraceFileFormat::VectorAsc, TraceFileFormat::CanDump,
                                          TraceFileFormat::PcapNg })
    {
        const QString folder = out.filePath(QStringLiteral("recorder_") + traceFormatName(format));
        QDir(folder).removeRecursively();
        if (!record(folder, format))
        {
            std::fprintf(stderr, "recording %s failed\n", qPrintable(traceFormatName(format)));
            return 1;
        }
    }
    return 0;
}
