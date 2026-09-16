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

unix:PKGCONFIG += libusb-1.0
win32:LIBS += -lusb-1.0
