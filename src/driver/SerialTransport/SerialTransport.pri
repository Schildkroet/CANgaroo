CONFIG += c++20
QT += serialport

HEADERS += \
    $$PWD/SerialTransport.h \
    $$PWD/QtSerialTransport.h

SOURCES += \
    $$PWD/QtSerialTransport.cpp
