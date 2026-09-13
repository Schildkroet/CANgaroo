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

#pragma once

#include <atomic>
#include <functional>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/BusMessage.h"
#include "core/TraceFileFormat.h"

struct RecordingConfig
{
    QString folder;
    QString fileNamePattern = QStringLiteral("trace_{date}_{time}");
    TraceFileFormat format = TraceFileFormat::VectorAsc;
    int splitSizeMb = 0;    // 0 = never start a new file
    bool stayArmed = true;  // keep recording armed for the next measurement
};

// Streams every trace message to disk while armed and a measurement runs, so
// long recordings are not limited by BusTrace's in-memory size.
//
// enqueue() is called from the bus listener threads and only copies the message
// into a queue; formatting and file I/O happen on the owning thread in batches.
class TraceRecorder : public QObject
{
    Q_OBJECT

public:
    using InterfaceNameFn = std::function<QString(BusInterfaceId)>;
    using MeasurementRunningFn = std::function<bool()>;

    // `interfaceName` labels candump lines and pcapng interfaces;
    // `measurementRunning` decides whether arming starts recording immediately.
    // Taking these instead of Backend keeps the recorder unit-testable.
    TraceRecorder(InterfaceNameFn interfaceName, MeasurementRunningFn measurementRunning, QObject *parent = nullptr);
    ~TraceRecorder() override;

    [[nodiscard]] static bool isFormatSupported(TraceFileFormat format) noexcept;

    // File name (without folder) of part `index` (1-based) of a recording that
    // started at `start`. Placeholders: {date}, {time}, {index}.
    [[nodiscard]] static QString fileNameFor(const RecordingConfig &config, const QDateTime &start, int index);

    // Takes effect with the next recording.
    void setConfig(const RecordingConfig &config);
    [[nodiscard]] const RecordingConfig &config() const noexcept { return _config; }

    void setArmed(bool armed);
    [[nodiscard]] bool isArmed() const noexcept { return _armed; }
    [[nodiscard]] bool isRecording() const noexcept { return _recording.load(); }

    [[nodiscard]] QString currentFilePath() const { return _file.fileName(); }
    [[nodiscard]] QDateTime recordingStartTime() const { return _sessionStart; }
    [[nodiscard]] qint64 totalBytesWritten() const noexcept { return _totalBytes; }
    [[nodiscard]] quint64 framesWritten() const noexcept { return _framesWritten; }

    // Called by Backend before the listeners start / after they have finished,
    // so no frame at either end of a measurement is missed.
    void onMeasurementStarting();
    void onMeasurementStopped();

    // Thread-safe.
    void enqueue(const BusMessage &msg);

signals:
    void armedChanged(bool armed);
    void recordingStarted(const QString &filePath);
    void recordingStopped();
    void fileRotated(const QString &filePath);

private slots:
    void drain();

private:
    static constexpr int drain_interval_ms = 250;
    static constexpr qsizetype max_pending = 1'000'000;

    InterfaceNameFn _interfaceName;
    MeasurementRunningFn _measurementRunning;
    RecordingConfig _config;
    RecordingConfig _active;  // snapshot of _config for the running recording

    bool _armed = false;
    std::atomic<bool> _recording{false};

    QMutex _pendingMutex;
    QVector<BusMessage> _pending;
    quint64 _dropped = 0;

    QTimer _drainTimer;
    QFile _file;
    QString _lastError;
    QDateTime _sessionStart;
    QMap<BusInterfaceId, int> _channelMap;
    QMap<BusInterfaceId, quint32> _pcapInterfaces;  // IDB index per interface in the current file
    double _fileStartTimestamp = 0.0;
    int _fileIndex = 0;
    qint64 _fileBytes = 0;
    quint64 _fileFrames = 0;
    qint64 _totalBytes = 0;
    quint64 _framesWritten = 0;

    bool startRecording();
    void stopRecording();
    void reportFailure(const QString &reason);

    [[nodiscard]] QVector<BusMessage> takePending();
    [[nodiscard]] bool writeMessages(const QVector<BusMessage> &messages);
    [[nodiscard]] bool writeChunk(const QByteArray &chunk);
    [[nodiscard]] bool openNextFile();
    [[nodiscard]] bool finishFile();
    [[nodiscard]] QString nextFilePath() const;
    [[nodiscard]] int channelFor(BusInterfaceId id);
};
