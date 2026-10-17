// Link stubs for MeasurementSetup / MeasurementNetwork / MeasurementInterface.
//
// Those classes reach into Backend and BusInterface only for XML load/save and
// database reload, none of which the tests exercise. Linking the real Backend
// would pull in every driver and the Python engine, so provide inert versions.

#include "core/Backend.h"
#include "driver/BusInterface.h"

BusInterface *Backend::getInterfaceByDriverAndName(QString, QString) { return nullptr; }
pCanDb Backend::loadDbc(QString, QString *) { return {}; }
pLinDb Backend::loadLdf(QString, QString *) { return {}; }

BusInterfaceId BusInterface::getId() const { return 0; }
QString Backend::getDriverName(BusInterfaceId) { return {}; }
QString Backend::getInterfaceName(BusInterfaceId) { return {}; }
