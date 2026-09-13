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

#include "TraceRecorder.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMutexLocker>

#include "core/Log.h"
#include "core/PcapNgFormat.h"
#include "core/TraceLineFormat.h"

TraceRecorder::TraceRecorder(InterfaceNameFn interfaceName, MeasurementRunningFn measurementRunning, QObject *parent)
  : QObject(parent),
    _interfaceName(std::move(interfaceName)),
    _measurementRunning(std::move(measurementRunning))
{
    _drainTimer.setInterval(drain_interval_ms);
    connect(&_drainTimer, &QTimer::timeout, this, &TraceRecorder::drain);
}

TraceRecorder::~TraceRecorder()
{
    // Normally finished by onMeasurementStopped(). Close quietly here: logging
    // would reach into a Backend that is already being destroyed.
    if (_recording.exchange(false))
    {
        [[maybe_unused]] const bool ok = writeMessages(takePending()) && finishFile();
    }
}

bool TraceRecorder::isFormatSupported(TraceFileFormat format) noexcept
{
    return format == TraceFileFormat::VectorAsc
        || format == TraceFileFormat::CanDump
        || format == TraceFileFormat::PcapNg;
}

QString TraceRecorder::fileNameFor(const RecordingConfig &config, const QDateTime &start, int index)
{
    QString name = config.fileNamePattern.trimmed();
    if (name.isEmpty())
    {
        name = RecordingConfig{}.fileNamePattern;
    }
    // Split parts need distinct names even if the pattern does not ask for it.
    if (config.splitSizeMb > 0 && !name.contains(QLatin1String("{index}")))
    {
        name += QLatin1String("_{index}");
    }

    name.replace(QLatin1String("{date}"), start.toString(QStringLiteral("yyyyMMdd")));
    name.replace(QLatin1String("{time}"), start.toString(QStringLiteral("HHmmss")));
    name.replace(QLatin1String("{index}"), QString::number(index).rightJustified(3, QLatin1Char('0')));
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    name.replace(QLatin1Char('\\'), QLatin1Char('_'));

    return name + QLatin1Char('.') + traceFormatExtension(config.format);
}

void TraceRecorder::setConfig(const RecordingConfig &config)
{
    _config = config;
}

void TraceRecorder::setArmed(bool armed)
{
    if (armed == _armed)
    {
        return;
    }
    _armed = armed;
    emit armedChanged(armed);

    if (!armed)
    {
        stopRecording();
    }
    else if (_measurementRunning && _measurementRunning())
    {
        startRecording();
    }
}

void TraceRecorder::onMeasurementStarting()
{
    if (_armed)
    {
        startRecording();
    }
}

void TraceRecorder::onMeasurementStopped()
{
    stopRecording();
    if (_armed && !_config.stayArmed)
    {
        setArmed(false);
    }
}

void TraceRecorder::enqueue(const BusMessage &msg)
{
    if (!_recording.load(std::memory_order_relaxed))
    {
        return;
    }

    QMutexLocker locker(&_pendingMutex);
    if (_pending.size() >= max_pending)
    {
        ++_dropped;
        return;
    }
    _pending.append(msg);
}

void TraceRecorder::drain()
{
    if (!_recording)
    {
        return;
    }
    if (!writeMessages(takePending()))
    {
        reportFailure(_lastError);
    }
}

bool TraceRecorder::startRecording()
{
    if (_recording)
    {
        return true;
    }

    _active = _config;
    if (!isFormatSupported(_active.format))
    {
        reportFailure(tr("%1 cannot be recorded continuously").arg(traceFormatName(_active.format)));
        return false;
    }

    {
        QMutexLocker locker(&_pendingMutex);
        _pending.clear();
        _dropped = 0;
    }
    _sessionStart = QDateTime::currentDateTime();
    _channelMap.clear();
    _fileIndex = 0;
    _totalBytes = 0;
    _framesWritten = 0;

    if (!openNextFile())
    {
        reportFailure(_lastError);
        return false;
    }

    _recording = true;
    _drainTimer.start();

    log_info(tr("Recording trace to %1").arg(_file.fileName()));
    emit recordingStarted(_file.fileName());
    return true;
}

void TraceRecorder::stopRecording()
{
    if (!_recording.exchange(false))
    {
        return;
    }
    _drainTimer.stop();

    if (!writeMessages(takePending()) || !finishFile())
    {
        reportFailure(_lastError);
        return;
    }

    log_info(tr("Recording stopped: %1 frames (%2) in %3 file(s)")
                 .arg(_framesWritten)
                 .arg(QLocale().formattedDataSize(_totalBytes))
                 .arg(_fileIndex));

    quint64 dropped = 0;
    {
        QMutexLocker locker(&_pendingMutex);
        dropped = _dropped;
    }
    if (dropped > 0)
    {
        log_warning(tr("Recording could not keep up with the bus: %1 frames were dropped").arg(dropped));
    }

    emit recordingStopped();
}

void TraceRecorder::reportFailure(const QString &reason)
{
    _recording = false;
    _drainTimer.stop();
    {
        QMutexLocker locker(&_pendingMutex);
        _pending.clear();
    }
    _file.close();

    log_error(tr("Trace recording failed: %1").arg(reason));
    emit recordingStopped();
    setArmed(false);
}

QVector<BusMessage> TraceRecorder::takePending()
{
    QMutexLocker locker(&_pendingMutex);
    return std::exchange(_pending, {});
}

bool TraceRecorder::writeMessages(const QVector<BusMessage> &messages)
{
    if (messages.isEmpty())
    {
        return true;
    }

    const qint64 splitBytes = qint64{_active.splitSizeMb} * 1024 * 1024;
    QByteArray chunk;

    for (const BusMessage &msg : messages)
    {
        if (splitBytes > 0 && _fileFrames > 0 && _fileBytes + chunk.size() >= splitBytes)
        {
            if (!writeChunk(chunk) || !finishFile() || !openNextFile())
            {
                return false;
            }
            chunk.clear();
            log_info(tr("Recording continues in %1").arg(_file.fileName()));
            emit fileRotated(_file.fileName());
        }

        switch (_active.format)
        {
            case TraceFileFormat::VectorAsc:
                if (_fileFrames == 0)
                {
                    // Like the one-shot export, times are relative to the file's first frame.
                    _fileStartTimestamp = msg.getFloatTimestamp();
                    chunk += TraceLineFormat::ascHeader(msg.getDateTime()).toUtf8();
                }
                chunk += TraceLineFormat::ascLine(msg, _fileStartTimestamp, channelFor(msg.getInterfaceId())).toUtf8();
                chunk += '\n';
                break;

            case TraceFileFormat::PcapNg:
            {
                if (_fileFrames == 0)
                {
                    chunk += PcapNgFormat::sectionHeaderBlock();
                }
                // Each file is its own section: interfaces are described the first
                // time they appear in it, before their first packet.
                const BusInterfaceId id = msg.getInterfaceId();
                auto it = _pcapInterfaces.constFind(id);
                if (it == _pcapInterfaces.cend())
                {
                    it = _pcapInterfaces.insert(id, static_cast<quint32>(_pcapInterfaces.size()));
                    chunk += PcapNgFormat::interfaceDescriptionBlock(_interfaceName(id));
                }
                chunk += PcapNgFormat::enhancedPacketBlock(msg, it.value());
                break;
            }

            default:
                chunk += TraceLineFormat::canDumpLine(msg, _interfaceName(msg.getInterfaceId())).toUtf8();
                chunk += '\n';
                break;
        }

        ++_fileFrames;
        ++_framesWritten;
    }

    if (!writeChunk(chunk))
    {
        return false;
    }
    // Flush every batch so a crash loses at most one drain interval.
    if (!_file.flush())
    {
        _lastError = tr("cannot write %1: %2").arg(_file.fileName(), _file.errorString());
        return false;
    }
    return true;
}

bool TraceRecorder::writeChunk(const QByteArray &chunk)
{
    if (chunk.isEmpty())
    {
        return true;
    }
    if (_file.write(chunk) != chunk.size())
    {
        _lastError = tr("cannot write %1: %2").arg(_file.fileName(), _file.errorString());
        return false;
    }
    _fileBytes += chunk.size();
    _totalBytes += chunk.size();
    return true;
}

bool TraceRecorder::openNextFile()
{
    if (_active.folder.isEmpty() || !QDir().mkpath(_active.folder))
    {
        _lastError = tr("cannot create folder '%1'").arg(_active.folder);
        return false;
    }

    ++_fileIndex;
    _file.setFileName(nextFilePath());
    if (!_file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        _lastError = tr("cannot open %1: %2").arg(_file.fileName(), _file.errorString());
        return false;
    }

    _fileBytes = 0;
    _fileFrames = 0;
    _pcapInterfaces.clear();
    return true;
}

bool TraceRecorder::finishFile()
{
    if (!_file.isOpen())
    {
        return true;
    }

    QByteArray tail;
    if (_active.format == TraceFileFormat::VectorAsc)
    {
        // A file without frames still gets a header so it is a valid ASC trace.
        if (_fileFrames == 0)
        {
            tail += TraceLineFormat::ascHeader(QDateTime::currentDateTime()).toUtf8();
        }
        tail += TraceLineFormat::ascFooter().toUtf8();
        tail += '\n';
    }
    else if (_active.format == TraceFileFormat::PcapNg && _fileFrames == 0)
    {
        // A pcapng file must at least contain its section header.
        tail += PcapNgFormat::sectionHeaderBlock();
    }

    const bool ok = writeChunk(tail) && _file.flush();
    if (!ok && _lastError.isEmpty())
    {
        _lastError = tr("cannot write %1: %2").arg(_file.fileName(), _file.errorString());
    }
    _file.close();
    return ok;
}

QString TraceRecorder::nextFilePath() const
{
    const QString fileName = fileNameFor(_active, _sessionStart, _fileIndex);
    const QString path = QDir(_active.folder).filePath(fileName);
    if (!QFileInfo::exists(path))
    {
        return path;
    }

    // Never overwrite an earlier recording: add a counter before the extension.
    const QFileInfo info(path);
    const QString base = QDir(_active.folder).filePath(info.completeBaseName());
    const QString ext = QLatin1Char('.') + info.suffix();
    QString candidate;
    for (int n = 2; ; ++n)
    {
        candidate = QStringLiteral("%1_%2%3").arg(base).arg(n).arg(ext);
        if (!QFileInfo::exists(candidate))
        {
            return candidate;
        }
    }
}

int TraceRecorder::channelFor(BusInterfaceId id)
{
    // Sequential channel numbers in order of first appearance, stable across split parts.
    const auto it = _channelMap.constFind(id);
    if (it != _channelMap.cend())
    {
        return it.value();
    }
    const int channel = static_cast<int>(_channelMap.size()) + 1;
    _channelMap.insert(id, channel);
    return channel;
}
