TARGET = ulc_driver_test
include(../common.pri)
INCLUDEPATH += $$PWD/..

QT += xml

# UlcInterface against a simulated uLC (FakeUlcDevice) behind the
# SerialTransport seam: no serial port, no hardware, no Qt SerialPort.
HEADERS += \
    FakeUlcDevice.h \
    ../support/FakeSerialPort.h \
    ../support/LogCapture.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/UlcDriver/UlcInterface.h

SOURCES += \
    UlcDriverTest.cpp \
    FakeUlcDevice.cpp \
    ../support/FakeSerialPort.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/UlcDriver/UlcInterface.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp \
    $$SRC_DIR/db/model/LinDb.cpp \
    $$SRC_DIR/db/model/LinFrame.cpp \
    $$SRC_DIR/db/model/LinSignal.cpp
