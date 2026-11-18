# Every GUI window/dialog. Each one is a self-contained directory with its own
# .pri listing its sources, headers and forms; add new ones here.

include($$PWD/TraceWindow/TraceWindow.pri)
include($$PWD/SetupDialog/SetupDialog.pri)
include($$PWD/LogWindow/LogWindow.pri)
include($$PWD/GraphWindow/GraphWindow.pri)
include($$PWD/CanStatusWindow/CanStatusWindow.pri)
include($$PWD/RawTxWindow/RawTxWindow.pri)
include($$PWD/TxGeneratorWindow/TxGeneratorWindow.pri)
include($$PWD/ScriptWindow/ScriptWindow.pri)
include($$PWD/ReplayWindow/ReplayWindow.pri)
include($$PWD/GatewayWindow/GatewayWindow.pri)
include($$PWD/LinControlWindow/LinControlWindow.pri)
include($$PWD/GpioControlWindow/GpioControlWindow.pri)
include($$PWD/SettingsDialog/SettingsDialog.pri)
include($$PWD/RecordingDialog/RecordingDialog.pri)
include($$PWD/ConditionalLoggingDialog/ConditionalLoggingDialog.pri)
include($$PWD/HardwareDialog/HardwareDialog.pri)
