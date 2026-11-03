TARGET = ulc_codec_test
include(../common.pri)

# UlcFrameCodec.h is header-only; it needs BusMessage only.
SOURCES += \
    UlcCodecTest.cpp \
    $$SRC_DIR/core/BusMessage.cpp
