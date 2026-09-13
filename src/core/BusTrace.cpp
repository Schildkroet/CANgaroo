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

#include "BusTrace.h"
#include <QMutexLocker>
#include <QFile>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/Log.h"
#include "core/TraceFileWriter.h"
#include "core/TraceRecorder.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"


BusTrace::BusTrace(Backend &backend, QObject *parent, int flushInterval)
  : QObject(parent),
    _backend(backend),
    _maxSize(50000),
    _isTimerRunning(false),
    _mutex(),
    _timerMutex(),
    _flushTimer(this)
{
    clear();
    _flushTimer.setSingleShot(true);
    _flushTimer.setInterval(flushInterval);
    connect(&_flushTimer, &QTimer::timeout, this, &BusTrace::flushQueue);
}

unsigned long BusTrace::size()
{
    QMutexLocker locker(&_mutex);
    return _dataRowsUsed;
}

void BusTrace::clear()
{
    QMutexLocker locker(&_mutex);
    emit beforeClear();
    _data.resize(pool_chunk_size);
    _dataRowsUsed = 0;
    _newRows = 0;
    _pruneWarned = false;
    _muxCache.clear();
    emit afterClear();
}

BusMessage BusTrace::getMessage(int idx)
{
    QMutexLocker locker(&_mutex);
    if (idx >= (_dataRowsUsed + _newRows)) {
        return BusMessage();
    } else {
        return _data[idx];
    }
}

QVector<BusMessage> BusTrace::getSnapshot(int maxCount)
{
    QMutexLocker locker(&_mutex);
    const int total = _dataRowsUsed + _newRows;
    const int start = (maxCount > 0 && maxCount < total) ? total - maxCount : 0;
    QVector<BusMessage> result;
    result.reserve(total - start);
    for (int i = start; i < total; i++)
    {
        result.append(_data.at(i));
    }
    return result;
}

void BusTrace::enqueueMessage(const BusMessage &msg, bool more_to_follow)
{
    // Outside _mutex: the recorder has its own lock and must see every frame,
    // including ones later pruned from the in-memory trace.
    if (auto *recorder = _backend.getTraceRecorder())
    {
        recorder->enqueue(msg);
    }

    QMutexLocker locker(&_mutex);

    int idx = _dataRowsUsed + _newRows;
    if (idx>=_data.size()) {
        _data.resize(_data.size() + pool_chunk_size);
    }

    _data[idx] = msg;
    _newRows++;

    if (!more_to_follow) {
        startTimer();
    }

    emit messageEnqueued(idx);
}

void BusTrace::flushQueue()
{
    {
        QMutexLocker locker(&_timerMutex);
        _isTimerRunning = false;
    }

    int toRemove = 0;
    int maxSize = 0;
    bool warnPrune = false;
    {
        QMutexLocker locker(&_mutex);
        if (!_newRows) {
            return;
        }

        // see if we have muxed messages. cache muxed values, if any.
        MeasurementSetup &setup = _backend.getSetup();
        for (int i=_dataRowsUsed; i<_dataRowsUsed + _newRows; i++) {
            BusMessage &msg = _data[i];
            CanDbMessage *dbmsg = setup.findDbMessage(msg);
            if (dbmsg && dbmsg->getMuxer()) {
                for (auto *signal : dbmsg->getSignals()) {
                    if (signal->isMuxed() && signal->isPresentInMessage(msg)) {
                        _muxCache[signal] = signal->extractRawDataFromMessage(msg);
                    }
                }
            }
        }

        _dataRowsUsed += _newRows;
        _newRows = 0;

        // Hard limit check - prune back below maxSize, with 10% headroom so we
        // don't prune on every flush. Covers a limit lowered below the current size.
        if (_dataRowsUsed > _maxSize) {
            toRemove = _dataRowsUsed - _maxSize + _maxSize / 10;
            maxSize = _maxSize;
            warnPrune = !_pruneWarned;
            _pruneWarned = true;
        }
    }

    // Warn once per trace, not on every prune, to keep the log readable.
    if (warnPrune) {
        log_warning(tr("Trace exceeded %1 messages; oldest messages are being discarded "
                       "and will be missing from saved traces. Increase the limit in Settings.")
                        .arg(maxSize));
    }

    // Signals are emitted without holding _mutex so connected models can
    // safely call back into size()/getMessage() while updating their views
    emit afterAppend();

    if (toRemove > 0) {
        emit beforeRemove(toRemove);
        {
            QMutexLocker locker(&_mutex);
            _data.remove(0, toRemove);
            _dataRowsUsed -= toRemove;
        }
        emit afterRemove(toRemove);
    }
}

void BusTrace::setMaxSize(int maxSize)
{
    QMutexLocker locker(&_mutex);
    _maxSize = maxSize;
}

void BusTrace::startTimer()
{
    QMutexLocker locker(&_timerMutex);
    if (!_isTimerRunning) {
        _isTimerRunning = true;
        QMetaObject::invokeMethod(&_flushTimer, "start", Qt::QueuedConnection);
    }
}

void BusTrace::save(QFile &file, TraceFileFormat format)
{
    QMutexLocker locker(&_mutex);
    const std::span<const BusMessage> messages(_data.constData(), static_cast<std::size_t>(_dataRowsUsed));
    TraceFileWriter::write(file, format, messages,
                           [this](BusInterfaceId id) { return _backend.getInterfaceName(id); });
}

bool BusTrace::getMuxedSignalFromCache(const CanDbSignal *signal, uint64_t *raw_value)
{
    auto it = _muxCache.constFind(signal);
    if (it != _muxCache.constEnd()) {
        *raw_value = it.value();
        return true;
    }
    return false;
}
