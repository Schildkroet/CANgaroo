TARGET = slcan_hw_compare
include(../common.pri)
INCLUDEPATH += $$PWD/..

# A tool, not a test: it needs a real adapter, so it is not in tests.pro.
CONFIG -= testcase
QT -= testlib
QT += serialport xml

HEADERS += \
    ../slcan_driver/FakeSlcanDevice.h \
    ../support/FakeSerialPort.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.h \
    $$SRC_DIR/driver/SerialTransport/QtSerialTransport.h

SOURCES += \
    main.cpp \
    ../slcan_driver/FakeSlcanDevice.cpp \
    ../support/FakeSerialPort.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/SerialTransport/QtSerialTransport.cpp \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
