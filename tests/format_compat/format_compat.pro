TARGET = trace_format_samples
include(../common.pri)

# Sample generator for validate.py, not a Qt Test binary: it is built and run by
# the format-compat CI job, not by "make check".
CONFIG -= testcase
QT -= testlib

HEADERS += $$SRC_DIR/core/TraceRecorder.h

SOURCES += \
    main.cpp \
    $$SRC_DIR/core/TraceFileWriter.cpp \
    $$SRC_DIR/core/TraceRecorder.cpp \
    $$SRC_DIR/core/TraceLineFormat.cpp \
    $$SRC_DIR/core/PcapNgFormat.cpp \
    $$SRC_DIR/core/BusMessage.cpp \
    $$PWD/../support/LogStub.cpp
