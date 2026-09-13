TARGET = trace_file_writer_test
include(../common.pri)

SOURCES += \
    TraceFileWriterTest.cpp \
    $$SRC_DIR/core/TraceFileWriter.cpp \
    $$SRC_DIR/core/TraceLineFormat.cpp \
    $$SRC_DIR/core/PcapNgFormat.cpp \
    $$SRC_DIR/core/BusMessage.cpp
