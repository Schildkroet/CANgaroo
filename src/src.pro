lessThan(QT_MAJOR_VERSION, 6): error("requires Qt 6")

QT += core gui
QT += widgets
QT += xml
QT += charts
QT += serialport
QT += svg

TARGET = cangaroo
TEMPLATE = app
CONFIG += warn_on
CONFIG += link_pkgconfig

VERSION = 0.7.0
DEFINES += VERSION_STRING=\\\"$${VERSION}\\\"

TRANSLATIONS = \
    translations/i18n_de_DE.ts \
    translations/i18n_zh_cn.ts \
    translations/i18n_es_ES.ts
RC_ICONS = cangaroo.ico

INCLUDEPATH += $$PWD

DESTDIR = ../bin
MOC_DIR = ../build/moc
RCC_DIR = ../build/rcc
UI_DIR = ../build/ui
unix:OBJECTS_DIR = ../build/o/unix
win32:OBJECTS_DIR = ../build/o/win32
macx:OBJECTS_DIR = ../build/o/mac


SOURCES += main.cpp\
    mainwindow.cpp \
    window/ConditionalLoggingDialog.cpp \
    window/SettingsDialog.cpp \
    window/RecordingDialog.cpp

HEADERS  += mainwindow.h \
    window/ConditionalLoggingDialog.h \
    window/SettingsDialog.h \
    window/RecordingDialog.h

FORMS    += mainwindow.ui

RESOURCES = cangaroo.qrc

include($$PWD/core/core.pri)
include($$PWD/driver/driver.pri)
include($$PWD/parser/dbc/dbc.pri)
include($$PWD/parser/ldf/ldf.pri)
include($$PWD/decoders/decoders.pri)
include($$PWD/window/TraceWindow/TraceWindow.pri)
include($$PWD/window/SetupDialog/SetupDialog.pri)
include($$PWD/window/LogWindow/LogWindow.pri)
include($$PWD/window/GraphWindow/GraphWindow.pri)
include($$PWD/window/CanStatusWindow/CanStatusWindow.pri)
include($$PWD/window/RawTxWindow/RawTxWindow.pri)
include($$PWD/window/TxGeneratorWindow/TxGeneratorWindow.pri)
include($$PWD/window/ScriptWindow/ScriptWindow.pri)
include($$PWD/window/ReplayWindow/ReplayWindow.pri)
include($$PWD/window/GatewayWindow/GatewayWindow.pri)
include($$PWD/window/LinControlWindow/LinControlWindow.pri)
include($$PWD/window/GpioControlWindow/GpioControlWindow.pri)
include($$PWD/helpers/helpers.pri)


PKGCONFIG += python3-embed
unix:INCLUDEPATH += /usr/include/pybind11
win32:INCLUDEPATH += $$system(python3 -c "import pybind11; print(pybind11.get_include())")

unix:PKGCONFIG += libnl-3.0
unix:PKGCONFIG += libnl-route-3.0
unix:INCLUDEPATH += /usr/include/libnl3
unix:include($$PWD/driver/SocketCanDriver/SocketCanDriver.pri)

include($$PWD/driver/CANBlastDriver/CANBlastDriver.pri)
include($$PWD/driver/SLCANDriver/SLCANDriver.pri)
include($$PWD/driver/GrIPDriver/GrIPDriver.pri)
# USB transport shared by the Linde and aiode drivers: libusb on Linux/macOS,
# WinUSB on Windows. Included once here, not from the driver .pri files.
include($$PWD/driver/UsbVendorInterface/UsbVendorInterface.pri)
include($$PWD/driver/LindeApiDriver/LindeApiDriver.pri)
include($$PWD/driver/AiodeDriver/AiodeDriver.pri)

win32:include($$PWD/driver/CandleApiDriver/CandleApiDriver.pri)

# Pass CONFIG+=peakcan to qmake to enable the PEAK PCAN driver.
# Requires the PCAN-Basic SDK extracted to src/driver/PeakCanDriver/pcan-basic-api/.
# Download: https://www.peak-system.com/fileadmin/media/files/PCAN-Basic.zip
win32:peakcan {
    DEFINES += PEAKCAN_DRIVER
    include($$PWD/driver/PeakCanDriver/PeakCanDriver.pri)
}

# Pass CONFIG+=kvaser to qmake to enable the Kvaser CANlib driver.
# Requires the Kvaser CANlib SDK (canlib.h + libcanlib / canlib32.dll).
# On Windows also set CANLIB_DIR=<path to SDK> (see KvaserDriver.pri).
kvaser {
    DEFINES += KVASER_DRIVER
    include($$PWD/driver/KvaserDriver/KvaserDriver.pri)
}

# Vector CAN driver — always enabled via Qt serialbus.
# Requires the Vector XL Driver Library installed on the target machine at runtime.
include($$PWD/driver/VectorDriver/VectorDriver.pri)

# TinyCAN driver — always enabled via Qt serialbus.
# Requires the TinyCAN driver/library installed on the target machine at runtime.
include($$PWD/driver/TinyCanDriver/TinyCanDriver.pri)

# Pass CONFIG+=zscanfd to qmake to enable the ZsCanFd driver.
# Requires the ZsCanFd.dll and custom qt_zscanfd canbus plugin on the target machine at runtime.
# Download zscanfd.dll: "https://files-accl.zohoexternal.in/public/workdrive-external/download/vg4j6fe5343da544d4d78876807400716c283?x-cli-msg=%7B%22linkId%22%3A%22BHvzgCTCwc-13uP6TI%22%2C%22isFileOwner%22%3Afalse%2C%22version%22%3A%221.0%22%2C%22isWDSupport%22%3Afalse%7D"
# Download qtzscanfdbus.dll: "https://files-accl.zohoexternal.in/public/workdrive-external/download/5twqz7da53b7da5804c958ca079ba061bd493?x-cli-msg=%7B%22linkId%22%3A%22BHvzgCTCw7-13uP6TI%22%2C%22isFileOwner%22%3Afalse%2C%22version%22%3A%221.0%22%2C%22isWDSupport%22%3Afalse%7D"
win32:zscanfd {
    DEFINES += ZSCANFD_DRIVER
    include($$PWD/driver/ZsCanFdDriver/ZsCanFdDriver.pri)
}

DISTFILES += \
    assets/filter-symbolic.svg
