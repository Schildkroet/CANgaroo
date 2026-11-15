TARGET = candle_driver_test
include(../common.pri)
INCLUDEPATH += $$PWD/..

QT += xml
QMAKE_CFLAGS += -std=gnu11

# The CandleAPI driver and its gs_usb protocol (api/candle.c) against
# simulated gs_usb adapters (FakeGsUsbDevice behind candle_usb.h). The WinUSB
# part (candle_usb_winusb.c) is replaced by FakeCandleUsb.cpp.
HEADERS += \
    FakeGsUsbDevice.h \
    ../support/LogCapture.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiInterface.h

SOURCES += \
    CandleDriverTest.cpp \
    FakeGsUsbDevice.cpp \
    FakeCandleUsb.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiInterface.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleSharedDevice.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiTiming.cpp \
    $$SRC_DIR/driver/CandleApiDriver/api/candle.c \
    $$SRC_DIR/driver/CandleApiDriver/api/candle_ctrl_req.c \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
