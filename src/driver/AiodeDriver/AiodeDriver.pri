CONFIG += c++20

SOURCES += \
    $$PWD/AiodeApi.cpp

HEADERS += \
    $$PWD/AiodeApi.hpp \
    $$PWD/aio_usb_protocol.h \
    $$PWD/../GpioProvider.h

unix:PKGCONFIG += libusb-1.0
win32:LIBS += -lusb-1.0
