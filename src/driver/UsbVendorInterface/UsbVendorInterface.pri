# One vendor interface of a composite USB device, opened on its own.
# Shared by LindeApiDriver (lin_usb) and AiodeDriver (aio_usb); include once.

HEADERS += \
    $$PWD/UsbVendorInterface.h

SOURCES += \
    $$PWD/UsbVendorInterface.cpp

unix {
    SOURCES += $$PWD/UsbVendorInterfaceLibusb.cpp
    # pkg-config supplies the include dir (<libusb.h> lives in .../include/libusb-1.0).
    PKGCONFIG += libusb-1.0
}

win32 {
    # WinUSB on the interface's own device node, found by DeviceInterfaceGUID.
    SOURCES += $$PWD/UsbVendorInterfaceWinUsb.cpp
    LIBS += -lsetupapi -lwinusb -lole32
}
