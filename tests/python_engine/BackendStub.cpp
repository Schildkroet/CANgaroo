// Link stubs for PythonEngine.
//
// PythonEngine reaches into Backend, BusTrace and BusInterface only from the
// functions of the embedded "cangaroo" module, which these tests don't call.
// Linking the real Backend would pull in every driver, so provide inert versions.

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "driver/BusInterface.h"

Backend::Backend()
    : QObject(nullptr)
    , _measurementRunning(false)
    , _measurementStartTime(0)
    , _setup(this)
    , _trace(nullptr)
    , _logModel(nullptr)
    , _conditionalLoggingManager(nullptr)
{
}

Backend::~Backend() = default;

void Backend::onMessageEnqueued(int) {}
bool Backend::startMeasurement() { return false; }
bool Backend::stopMeasurement() { return false; }
bool Backend::isMeasurementRunning() const { return false; }
MeasurementSetup &Backend::getSetup() { return _setup; }
BusTrace *Backend::getTrace() { return nullptr; }
void Backend::clearTrace() {}
CanDbMessage *Backend::findDbMessage(const BusMessage &) const { return nullptr; }
LinFrame *Backend::findLinFrame(const BusMessage &) const { return nullptr; }
BusInterfaceIdList Backend::getInterfaceList() { return {}; }
BusInterface *Backend::getInterfaceById(BusInterfaceId) { return nullptr; }
QString Backend::getInterfaceName(BusInterfaceId) { return {}; }
QString Backend::getDriverName(BusInterfaceId) { return {}; }
BusInterface *Backend::getInterfaceByDriverAndName(QString, QString) { return nullptr; }
pCanDb Backend::loadDbc(QString, QString *) { return {}; }
pLinDb Backend::loadLdf(QString, QString *) { return {}; }

QVector<BusMessage> BusTrace::getSnapshot(int) { return {}; }
void BusTrace::save(QFile &, TraceFileFormat) {}
unsigned long BusTrace::size() { return 0; }

BusInterfaceId BusInterface::getId() const { return 0; }
QString BusInterface::getStateText() { return {}; }
