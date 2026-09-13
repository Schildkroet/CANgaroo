TARGET = trace_line_format_test
include(../common.pri)

SOURCES += \
    TraceLineFormatTest.cpp \
    $$SRC_DIR/core/TraceLineFormat.cpp \
    $$SRC_DIR/core/BusMessage.cpp
