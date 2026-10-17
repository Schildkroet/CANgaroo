TARGET = measurement_setup_test
include(../common.pri)

QT += xml

HEADERS += \
    $$SRC_DIR/core/MeasurementSetup.h

SOURCES += \
    MeasurementSetupTest.cpp \
    BackendStub.cpp \
    ../support/LogStub.cpp \
    $$SRC_DIR/core/MeasurementSetup.cpp \
    $$SRC_DIR/core/MeasurementNetwork.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/db/dbc/DbcParser.cpp \
    $$SRC_DIR/db/dbc/DbcTokens.cpp \
    $$SRC_DIR/core/BusMessage.cpp \
    $$SRC_DIR/db/model/CanDb.cpp \
    $$SRC_DIR/db/model/CanDbMessage.cpp \
    $$SRC_DIR/db/model/CanDbNode.cpp \
    $$SRC_DIR/db/model/CanDbSignal.cpp \
    $$SRC_DIR/db/model/LinDb.cpp \
    $$SRC_DIR/db/model/LinFrame.cpp \
    $$SRC_DIR/db/model/LinSignal.cpp
