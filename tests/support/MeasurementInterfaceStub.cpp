// Link stubs for core/MeasurementInterface.cpp, which reaches into Backend only for its
// XML load/save. Linking the real Backend would pull in every driver and the
// Python engine; the tests never load or save a setup.

#include "core/Backend.h"

BusInterface *Backend::getInterfaceByDriverAndName(QString, QString) { return nullptr; }
QString Backend::getDriverName(BusInterfaceId) { return {}; }
QString Backend::getInterfaceName(BusInterfaceId) { return {}; }
