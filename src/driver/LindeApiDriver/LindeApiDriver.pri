CONFIG += c++20

SOURCES += \
    $$PWD/LindeApiDriver.cpp \
    $$PWD/LindeApiInterface.cpp \
    $$PWD/LindeSharedDevice.cpp

HEADERS += \
    $$PWD/LindeApiDriver.h \
    $$PWD/LindeApiInterface.h \
    $$PWD/LindeSharedDevice.h \
    $$PWD/lin_usb_protocol.h

# pkg-config supplies the include dir (<libusb.h> lives in .../include/libusb-1.0)
# and the link flags on Linux and MSYS2 alike.
PKGCONFIG += libusb-1.0
