CONFIG += c++20

SOURCES += \
    $$PWD/UlcInterface.cpp \
    $$PWD/UlcDriver.cpp

HEADERS  += \
    $$PWD/UlcFrameCodec.h \
    $$PWD/UlcInterface.h \
    $$PWD/UlcDriver.h

FORMS +=

win32:LIBS += -lsetupapi -lcfgmgr32
