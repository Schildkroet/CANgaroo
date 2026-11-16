TARGET = python_engine_test
include(../common.pri)

QT += xml

# Same embedding setup as src/src.pro.
CONFIG += link_pkgconfig
PKGCONFIG += python3-embed
unix:INCLUDEPATH += /usr/include/pybind11
win32 {
    PYBIND11_INCLUDE = $$system("python3 -c \"import pybind11; print(pybind11.get_include())\"")
    INCLUDEPATH += $$PYBIND11_INCLUDE
}

HEADERS += \
    $$SRC_DIR/core/PythonEngine.h \
    $$SRC_DIR/core/Backend.h \
    $$SRC_DIR/core/MeasurementSetup.h

SOURCES += \
    PythonEngineTest.cpp \
    BackendStub.cpp \
    ../support/LogStub.cpp \
    $$SRC_DIR/core/PythonEngine.cpp \
    $$SRC_DIR/core/MeasurementSetup.cpp \
    $$SRC_DIR/core/MeasurementNetwork.cpp \
    $$SRC_DIR/core/MeasurementInterface.cpp \
    $$SRC_DIR/core/BusMessage.cpp \
    $$SRC_DIR/db/dbc/DbcParser.cpp \
    $$SRC_DIR/db/dbc/DbcTokens.cpp \
    $$SRC_DIR/db/model/CanDb.cpp \
    $$SRC_DIR/db/model/CanDbMessage.cpp \
    $$SRC_DIR/db/model/CanDbNode.cpp \
    $$SRC_DIR/db/model/CanDbSignal.cpp \
    $$SRC_DIR/db/model/LinDb.cpp \
    $$SRC_DIR/db/model/LinFrame.cpp \
    $$SRC_DIR/db/model/LinSignal.cpp
