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

#include <stdint.h>
#include <atomic>
#include <QObject>
#include <QList>
#include <QMutex>
#include <QDateTime>
#include <QElapsedTimer>
#include "driver/CanDriver.h"
#include "core/BusMessage.h"
#include "core/DBC/CanDb.h"
#include "core/DBC/LinDb.h"
#include "core/MeasurementSetup.h"
#include "core/Log.h"
#include "core/ConditionalLoggingManager.h"

class MeasurementNetwork;
class BusTrace;
class BusListener;
class CanDbMessage;
class LinFrame;
class SetupDialog;
class LogModel;
class TraceRecorder;

class Backend : public QObject
{
    Q_OBJECT
public:
    static Backend &instance();

    explicit Backend();
    virtual ~Backend();

    void addCanDriver(CanDriver &driver);
    void refreshInterfaces();

    bool startMeasurement();
    bool stopMeasurement();
    bool isMeasurementRunning() const;
    double getTimestampAtMeasurementStart() const;
    uint64_t getUsecsAtMeasurementStart() const;
    uint64_t getNsecsSinceMeasurementStart() const;
    uint64_t getUsecsSinceMeasurementStart() const;


    void logMessage(const QDateTime dt, const log_level_t level, const QString msg);

    MeasurementSetup &getSetup();
    void loadDefaultSetup(MeasurementSetup &setup);
    void setDefaultSetup();
    void setSetup(MeasurementSetup &new_setup);

    double currentTimeStamp() const;

    BusTrace *getTrace();
    void clearTrace();

    ConditionalLoggingManager *getConditionalLoggingManager() const { return _conditionalLoggingManager; }
    TraceRecorder *getTraceRecorder() const { return _traceRecorder; }

    CanDbMessage *findDbMessage(const BusMessage &msg) const;
    LinFrame     *findLinFrame(const BusMessage &msg) const;

    BusInterfaceIdList getInterfaceList();
    CanDriver *getDriverById(BusInterfaceId id);
    BusInterface *getInterfaceById(BusInterfaceId id);
    QString getInterfaceName(BusInterfaceId id);
    QString getDriverName(BusInterfaceId id);

    CanDriver *getDriverByName(QString driverName);
    BusInterface *getInterfaceByDriverAndName(QString driverName, QString deviceName);

    pCanDb loadDbc(QString filename, QString *errorMsg = nullptr);
    pLinDb loadLdf(QString filename, QString *errorMsg = nullptr);

    void notifyDecoderConfigChanged();
    void notifyDisplayConfigChanged();

    void clearLog();
    LogModel &getLogModel() const;

signals:
    void beginMeasurement();
    void endMeasurement();

    void onSetupChanged();
    void onDisplayConfigChanged();

    void onClearTraceRequested();

    void onLogMessage(const QDateTime dt, const log_level_t level, const QString msg);

    void onSetupDialogCreated(SetupDialog &dlg);

public slots:
    void onMessageEnqueued(int idx);

private:
    static Backend *_instance;

    std::atomic<bool> _measurementRunning;
    uint64_t _measurementStartTime;
    QElapsedTimer _timerSinceStart;
    QList<CanDriver*> _drivers;
    MeasurementSetup _setup;
    BusTrace *_trace;
    QList<BusListener*> _listeners;

    LogModel *_logModel;
    ConditionalLoggingManager *_conditionalLoggingManager;
    TraceRecorder *_traceRecorder = nullptr;
};
