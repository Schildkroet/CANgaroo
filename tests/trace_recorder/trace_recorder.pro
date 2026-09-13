TARGET = trace_recorder_test
include(../common.pri)

# TraceRecorder is a QObject; list its header so moc runs.
HEADERS += $$SRC_DIR/core/TraceRecorder.h

SOURCES += \
    TraceRecorderTest.cpp \
    $$SRC_DIR/core/TraceRecorder.cpp \
    $$SRC_DIR/core/TraceLineFormat.cpp \
    $$SRC_DIR/core/PcapNgFormat.cpp \
    $$SRC_DIR/core/BusMessage.cpp \
    $$PWD/../support/LogStub.cpp
