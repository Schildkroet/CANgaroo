TARGET = slcan_driver_test
include(../common.pri)
INCLUDEPATH += $$PWD/..

QT += xml

# SLCANInterface against simulated CANable 1 / CANable 2 / WeAct adapters
# (FakeSlcanDevice) behind the SerialTransport seam: no Qt SerialPort.
HEADERS += \
    FakeSlcanDevice.h \
    ../support/FakeSerialPort.h \
    ../support/LogCapture.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.h

SOURCES += \
    SlcanDriverTest.cpp \
    FakeSlcanDevice.cpp \
    ../support/FakeSerialPort.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
