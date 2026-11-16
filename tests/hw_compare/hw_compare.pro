TARGET = slcan_hw_compare
include(../common.pri)
INCLUDEPATH += $$PWD/..

# A tool, not a test: it needs a real adapter, so it is not in tests.pro.
CONFIG -= testcase
QT -= testlib
QT += serialport xml
QMAKE_CFLAGS += -std=gnu11
CONFIG += link_pkgconfig
PKGCONFIG += libusb-1.0

HEADERS += \
    linusb_compare.h \
    ../lindeapi_driver/FakeLinUsbDevice.h \
    $$SRC_DIR/driver/LindeApiDriver/LindeApiInterface.h \
    ../slcan_driver/FakeSlcanDevice.h \
    ../candle_driver/FakeGsUsbDevice.h \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiInterface.h \
    ../support/FakeSerialPort.h \
    $$SRC_DIR/driver/BusInterface.h \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.h \
    $$SRC_DIR/driver/SerialTransport/QtSerialTransport.h

SOURCES += \
    main.cpp \
    linusb_compare.cpp \
    ../lindeapi_driver/FakeLinUsbDevice.cpp \
    $$SRC_DIR/driver/LindeApiDriver/LindeApiInterface.cpp \
    $$SRC_DIR/driver/LindeApiDriver/LindeSharedDevice.cpp \
    $$SRC_DIR/driver/UsbVendorInterface/UsbVendorInterface.cpp \
    $$SRC_DIR/driver/UsbVendorInterface/UsbVendorInterfaceLibusb.cpp \
    $$SRC_DIR/db/model/LinDb.cpp \
    $$SRC_DIR/db/model/LinFrame.cpp \
    $$SRC_DIR/db/model/LinSignal.cpp \
    ../slcan_driver/FakeSlcanDevice.cpp \
    ../candle_driver/FakeGsUsbDevice.cpp \
    candle_usb_hw.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiInterface.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleSharedDevice.cpp \
    $$SRC_DIR/driver/CandleApiDriver/CandleApiTiming.cpp \
    $$SRC_DIR/driver/CandleApiDriver/api/candle.c \
    $$SRC_DIR/driver/CandleApiDriver/api/candle_ctrl_req.c \
    ../support/FakeSerialPort.cpp \
    ../support/LogCapture.cpp \
    ../support/MeasurementInterfaceStub.cpp \
    $$SRC_DIR/driver/SerialTransport/QtSerialTransport.cpp \
    $$SRC_DIR/driver/SLCANDriver/SLCANInterface.cpp \
    $$SRC_DIR/driver/BusInterface.cpp \
    $$SRC_DIR/driver/CanTiming.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp
