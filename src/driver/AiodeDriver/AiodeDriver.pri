CONFIG += c++20

SOURCES += \
    $$PWD/AiodeApi.cpp

HEADERS += \
    $$PWD/AiodeApi.hpp \
    $$PWD/aio_usb_protocol.h \
    $$PWD/../GpioProvider.h

# pkg-config supplies the include dir (<libusb.h> lives in .../include/libusb-1.0)
# and the link flags on Linux and MSYS2 alike.
PKGCONFIG += libusb-1.0
