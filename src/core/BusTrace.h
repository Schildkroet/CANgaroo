/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include <QObject>
#include <QMutex>

#include "core/TraceFileFormat.h"
#include <QTimer>
#include <QVector>
#include <QMap>
#include <QFile>

#include "BusMessage.h"

class BusInterface;
class CanDbMessage;
class CanDbSignal;
class LinSignal;
class MeasurementSetup;
class Backend;

class BusTrace : public QObject
{
    Q_OBJECT

public:
    explicit BusTrace(Backend &backend, QObject *parent, int flushInterval);

    unsigned long size();
    void clear();
    BusMessage getMessage(int idx);
    QVector<BusMessage> getSnapshot(int maxCount = 0);
    void enqueueMessage(const BusMessage &msg, bool more_to_follow=false);
    void setMaxSize(int maxSize);

    // Writes the whole trace in `format`; see core/TraceFileWriter.
    void save(QFile &file, TraceFileFormat format);

    bool getMuxedSignalFromCache(const CanDbSignal *signal, uint64_t *raw_value);

    // LIN mux cache not needed (LIN has no multiplexing), but keep symmetry:
    bool getLinSignalFromCache(const LinSignal *signal, uint64_t *raw_value) { (void)signal; (void)raw_value; return false; }

signals:
    void messageEnqueued(int idx);
    void afterAppend();
    void beforeRemove(int count);
    void afterRemove(int count);
    void beforeClear();
    void afterClear();

private slots:
    void flushQueue();

private:
    enum {
        pool_chunk_size = 1024
    };

    Backend &_backend;

    QVector<BusMessage> _data;
    int _dataRowsUsed;
    int _newRows;
    int _maxSize;
    bool _isTimerRunning;
    bool _pruneWarned = false;

    QMap<const CanDbSignal*,uint64_t> _muxCache;

    QRecursiveMutex _mutex;
    QMutex _timerMutex;
    QTimer _flushTimer;

    void startTimer();


};
