TARGET = pcapng_format_test
include(../common.pri)

SOURCES += \
    PcapNgFormatTest.cpp \
    $$SRC_DIR/core/PcapNgFormat.cpp \
    $$SRC_DIR/core/BusMessage.cpp
