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

#include "Backend.h"
#include "LogModel.h"

#include <QDateTime>
#include <QFileInfo>

#include "core/BusTrace.h"
#include "core/TraceRecorder.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/CanDriver.h"
#include "driver/BusInterface.h"
#include "driver/BusListener.h"
#include "parser/dbc/DbcParser.h"
#include "core/DBC/LinDb.h"
#include "core/DBC/LinFrame.h"

Backend *Backend::_instance = nullptr;

Backend::Backend()
  : QObject(0),
    _measurementRunning(false),
    _measurementStartTime(0),
    _setup(this)
{
    _logModel = new LogModel(*this);

    setDefaultSetup();
    _trace = new BusTrace(*this, this, 50);
    _conditionalLoggingManager = new ConditionalLoggingManager(*this, this);
    _traceRecorder = new TraceRecorder(
        [this](BusInterfaceId id) { return getInterfaceName(id); },
        [this]() { return isMeasurementRunning(); },
        this);

    connect(_trace, &BusTrace::messageEnqueued, this, &Backend::onMessageEnqueued);
    connect(&_setup, &MeasurementSetup::onSetupChanged, this, &Backend::onSetupChanged);
}

Backend &Backend::instance()
{
    if (!_instance) {
        _instance = new Backend;
    }
    return *_instance;
}

Backend::~Backend()
{
    delete _trace;
    qDeleteAll(_drivers);
}

void Backend::addCanDriver(CanDriver &driver)
{
    driver.init(_drivers.size());
    _drivers.append(&driver);
}

void Backend::refreshInterfaces()
{
    for (auto *driver : _drivers) {
        driver->update();
    }
    emit onSetupChanged();
}

void Backend::notifyDecoderConfigChanged()
{
    emit onSetupChanged();
}

void Backend::notifyDisplayConfigChanged()
{
    emit onDisplayConfigChanged();
}

bool Backend::startMeasurement()
{
    log_info(tr("Starting measurement"));

    _measurementStartTime = QDateTime::currentMSecsSinceEpoch();
    _timerSinceStart.start();

    // Before the listeners start, so the recording includes the first frames.
    _traceRecorder->onMeasurementStarting();

    for (auto *network : _setup.getNetworks()) {
        for (auto *mi : network->interfaces()) {

            if (!mi->isEnabled()) { continue; }

            BusInterface *intf = getInterfaceById(mi->busInterface());
            if (intf) {
                intf->applyConfig(*mi);

                log_info(QString(tr("Listening on interface: %1")).arg(intf->getName()));
                BusListener *listener = new BusListener(0, *this, *intf);
                listener->startThread();
                _listeners.append(listener);
            }
        }
    }

    _measurementRunning = true;
    emit beginMeasurement();
    return true;
}

bool Backend::stopMeasurement()
{
    if (_measurementRunning) {
        for (auto *listener : _listeners) {
            listener->requestStop();
        }

        for (auto *listener : _listeners) {
            log_info(QString(tr("Closing interface: %1")).arg(getInterfaceName(listener->getInterfaceId())));
            listener->waitFinish();
        }

        qDeleteAll(_listeners);
        _listeners.clear();

        // Listeners are joined, so every frame has reached the recorder.
        _traceRecorder->onMeasurementStopped();

        log_info(tr("Measurement stopped"));

        _measurementRunning = false;

        emit endMeasurement();
    }
    return true;
}

bool Backend::isMeasurementRunning() const
{
    return _measurementRunning;
}

void Backend::loadDefaultSetup(MeasurementSetup &setup)
{
    setup.clear();
    int i = 1;

    for (auto *driver : _drivers) {
        driver->update();
        for (auto intf : driver->getInterfaceIds()) {
            MeasurementNetwork *network = setup.createNetwork();
            network->setName(tr("Network ") + QString("%1").arg(i++));

            MeasurementInterface *mi = new MeasurementInterface();
            mi->setBusInterface(intf);
            mi->setResolved(true);
            BusInterface *canIntf = getInterfaceById(intf);
            if (canIntf)
                mi->setBusType(canIntf->busType());
            mi->setBitrate(500000);
            mi->setFdBitrate(2000000);
            network->addInterface(mi);
        }
    }
}

void Backend::setDefaultSetup()
{
    loadDefaultSetup(_setup);
    emit onSetupChanged();
}

MeasurementSetup &Backend::getSetup()
{
    return _setup;
}

void Backend::setSetup(MeasurementSetup &new_setup)
{
    _setup.cloneFrom(new_setup);
    emit onSetupChanged();
}

double Backend::currentTimeStamp() const
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000;
}

BusTrace *Backend::getTrace()
{
    return _trace;
}

void Backend::clearTrace()
{
    _trace->clear();
    emit onClearTraceRequested();
}

CanDbMessage *Backend::findDbMessage(const BusMessage &msg) const
{
    return _setup.findDbMessage(msg);
}

LinFrame *Backend::findLinFrame(const BusMessage &msg) const
{
    return _setup.findLinFrame(msg);
}

BusInterfaceIdList Backend::getInterfaceList()
{
    BusInterfaceIdList result;
    for (auto *driver : _drivers) {
        for (auto id : driver->getInterfaceIds()) {
            result.append(id);
        }
    }
    return result;
}

CanDriver *Backend::getDriverById(BusInterfaceId id)
{
    if (id == InvalidBusInterfaceId)
        return nullptr;
    int driverIdx = (id >> 8) & 0xFF;
    if (driverIdx >= _drivers.size())
    {
        log_critical(QString(tr("Unable to get driver for interface id: %1. This should never happen.")).arg(QString().number(id)));
        return nullptr;
    }
    return _drivers.value(driverIdx);
}

BusInterface *Backend::getInterfaceById(BusInterfaceId id)
{
    CanDriver *driver = getDriverById(id);
    return driver ? driver->getInterfaceById(id) : nullptr;
}

QString Backend::getInterfaceName(BusInterfaceId id)
{
    BusInterface *intf = getInterfaceById(id);
    return intf ? intf->getName() : QString::number(id);
}

QString Backend::getDriverName(BusInterfaceId id)
{
    CanDriver *driver = getDriverById(id);
    return driver ? driver->getName() : "";
}

CanDriver *Backend::getDriverByName(QString driverName)
{
    for (auto *driver : _drivers) {
        if (driver->getName()==driverName) {
            return driver;
        }
    }
    return 0;
}

BusInterface *Backend::getInterfaceByDriverAndName(QString driverName, QString deviceName)
{
    CanDriver *driver = getDriverByName(driverName);
    if (driver) {
        return driver->getInterfaceByName(deviceName);
    } else {
        return 0;
    }

}

pLinDb Backend::loadLdf(QString filename, QString *errorMsg)
{
    QFileInfo info(filename);
    if (!info.exists() || !info.isReadable()) {
        if (errorMsg) *errorMsg = tr("File not found or not readable.");
        return pLinDb();
    }
    pLinDb lindb(new LinDb());
    if (!lindb->loadFile(filename)) {
        if (errorMsg) *errorMsg = lindb->lastError();
        return pLinDb();
    }
    return lindb;
}

pCanDb Backend::loadDbc(QString filename, QString *errorMsg)
{
    DbcParser parser;

    QFileInfo info(filename);
    if (!info.exists() || !info.isReadable()) {
        if (errorMsg) {
            *errorMsg = tr("File not found or not readable.");
        }
        return pCanDb();
    }

    QFile *dbc = new QFile(filename);

    pCanDb candb(new CanDb());
    if (!parser.parseFile(dbc, *candb)) {
        if (errorMsg) {
            *errorMsg = tr("Failed to parse DBC file. Please check the log for details.");
        }
        delete dbc;
        return pCanDb();
    }
    delete dbc;

    return candb;
}

void Backend::clearLog()
{
    _logModel->clear();
}

LogModel &Backend::getLogModel() const
{
    return *_logModel;
}

double Backend::getTimestampAtMeasurementStart() const
{
    return static_cast<double>(_measurementStartTime) / 1000.0;
}

uint64_t Backend::getUsecsAtMeasurementStart() const
{
    return _measurementStartTime * 1000;
}

uint64_t Backend::getNsecsSinceMeasurementStart() const
{
    return _timerSinceStart.nsecsElapsed();
}

uint64_t Backend::getUsecsSinceMeasurementStart() const
{
    return getNsecsSinceMeasurementStart() / 1000;
}

void Backend::logMessage(const QDateTime dt, const log_level_t level, const QString msg)
{
    emit onLogMessage(dt, level, msg);
}

void Backend::onMessageEnqueued(int idx)
{
    if (_conditionalLoggingManager->isEnabled()) {
        _conditionalLoggingManager->processMessage(_trace->getMessage(idx));
    }
}
