TARGET = lindeapi_driver_test
include(../common.pri)
INCLUDEPATH += $$PWD/..

QT += xml

# The LindeAPI driver (LindeApiInterface, LindeSharedDevice) against a
# simulated lin_usb device (FakeLinUsbDevice, from the firmware's lin_usb.c).
# FakeUsbVendorInterface.cpp replaces UsbVendorInterfaceLibusb.cpp.
HEADERS += \
    FakeLinUsbDevice.h \
    ../support/LogCapture.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/LindeApiDriver/LindeApiInterface.h

SOURCES += \
    LindeApiDriverTest.cpp \
    FakeLinUsbDevice.cpp \
    FakeUsbVendorInterface.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/LindeApiDriver/LindeApiInterface.cpp \
    $$SRC_DIR/driver/LindeApiDriver/LindeSharedDevice.cpp \
    $$SRC_DIR/driver/UsbVendorInterface/UsbVendorInterface.cpp \
    $$SRC_DIR/db/model/LinDb.cpp \
    $$SRC_DIR/db/model/LinFrame.cpp \
    $$SRC_DIR/db/model/LinSignal.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
