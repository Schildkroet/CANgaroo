TARGET = canblaster_driver_test
include(../common.pri)
INCLUDEPATH += $$PWD/..

QT += network xml

# CANBlasterInterface against a CANblaster server on localhost UDP
# (FakeCanblasterServer, from canblaster.py). Uses UDP ports 20001 / 20002.
HEADERS += \
    FakeCanblasterServer.h \
    ../support/LogCapture.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/CANBlastDriver/CANBlasterInterface.h

SOURCES += \
    CanblasterDriverTest.cpp \
    FakeCanblasterServer.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/CANBlastDriver/CANBlasterInterface.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
