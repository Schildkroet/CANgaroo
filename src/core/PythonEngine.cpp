/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

// pybind11/Python must come before Qt headers to avoid "slots" macro clash
#undef slots
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#define slots Q_SLOTS

#include "PythonEngine.h"

#include <optional>

#include "core/AutosarE2E.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QStandardPaths>

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/DBC/CanDb.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"
#include "core/DBC/LinDb.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/LinSignal.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/Log.h"
#include "driver/BusInterface.h"

namespace py = pybind11;
using namespace py::literals;

// ---------------------------------------------------------------------------
// Global pointer so the embedded module can reach the active engine
// ---------------------------------------------------------------------------
static PythonEngine *g_activeEngine = nullptr;

// ---------------------------------------------------------------------------
// Helper: find a CanDbMessage by name across all loaded DBs
// ---------------------------------------------------------------------------
static CanDbMessage *findDbMessageByName(Backend &backend, const QString &name)
{
    MeasurementSetup &setup = backend.getSetup();
    for (MeasurementNetwork *net : setup.getNetworks())
    {
        for (const pCanDb &db : std::as_const(net->_canDbs))
        {
            for (CanDbMessage *dbMsg : db->getMessageList())
            {
                if (dbMsg->getName() == name)
                {
                    return dbMsg;
                }
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: build the signal-definition dict used by lookup() and find_message()
// ---------------------------------------------------------------------------
static py::dict buildMessageDict(CanDbMessage *dbMsg)
{
    py::list sigList;
    for (CanDbSignal *sig : dbMsg->getSignals())
    {
        py::dict s;
        s["name"]         = sig->name().toStdString();
        s["start_bit"]    = sig->startBit();
        s["length"]       = sig->length();
        s["is_big_endian"]= sig->isBigEndian();
        s["is_unsigned"]  = sig->isUnsigned();
        s["factor"]       = sig->getFactor();
        s["offset"]       = sig->getOffset();
        s["min"]          = sig->getMinimumValue();
        s["max"]          = sig->getMaximumValue();
        s["unit"]         = sig->getUnit().toStdString();
        s["comment"]      = sig->comment().toStdString();
        if (sig->isMuxed())  { s["mux_value"] = sig->getMuxValue(); }
        if (sig->isMuxer())  { s["is_muxer"]  = true; }
        sigList.append(s);
    }

    py::dict result;
    result["message"] = dbMsg->getName().toStdString();
    result["id"]      = dbMsg->getRaw_id();
    result["dlc"]     = dbMsg->getDlc();
    result["comment"] = dbMsg->getComment().toStdString();
    result["signals"] = sigList;

    CanDbNode *sender = dbMsg->getSender();
    if (sender) { result["sender"] = sender->name().toStdString(); }

    return result;
}

// ---------------------------------------------------------------------------
// Embedded "cangaroo" Python module
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE(cangaroo, m)
{
    // --- BusMessage binding ---
    py::class_<BusMessage>(m, "Message")
        .def(py::init<>())
        .def(py::init<uint32_t>())
        .def_property("id",        &BusMessage::getId,       &BusMessage::setId)
        .def_property("dlc",       &BusMessage::getLength,   &BusMessage::setLength)
        .def_property("extended",  &BusMessage::isExtended,  &BusMessage::setExtended)
        .def_property("fd",        &BusMessage::isFD,        &BusMessage::setFD)
        .def_property("rtr",       &BusMessage::isRTR,       &BusMessage::setRTR)
        .def_property("brs",       &BusMessage::isBRS,       &BusMessage::setBRS)
        .def_property_readonly("interface_id", &BusMessage::getInterfaceId)
        .def_property_readonly("timestamp",    &BusMessage::getFloatTimestamp)
        .def_property_readonly("is_rx",        &BusMessage::isRX)
        .def_property_readonly("is_lin_sleep",  &BusMessage::isLinSleepFrame)
        .def_property_readonly("is_lin_wakeup", &BusMessage::isLinWakeupFrame)
        .def("get_byte", &BusMessage::getByte)
        .def("set_byte", &BusMessage::setByte)
        .def("get_data", [](const BusMessage &msg) -> py::bytes
        {
            std::string data(msg.getLength(), '\0');
            for (int i = 0; i < msg.getLength(); i++)
            {
                data[i] = static_cast<char>(msg.getByte(i));
            }
            return py::bytes(data);
        })
        .def("set_data", [](BusMessage &msg, py::bytes data)
        {
            std::string s = data;
            msg.setLength(static_cast<uint8_t>(std::min<size_t>(s.size(), 64)));
            for (size_t i = 0; i < s.size() && i < 64; i++)
            {
                msg.setByte(i, static_cast<uint8_t>(s[i]));
            }
        })
        .def_property("bustype",
            [](const BusMessage &msg) -> std::string
            {
                return msg.busType() == BusType::LIN ? "LIN" : "CAN";
            },
            [](BusMessage &msg, const std::string &s)
            {
                msg.setBusType(s == "LIN" ? BusType::LIN : BusType::CAN);
            })
        .def("__repr__", [](const BusMessage &msg) -> std::string
        {
            const std::string type = msg.busType() == BusType::LIN ? "LinMessage" : "Message";
            return "<cangaroo." + type + " id=0x"
                   + msg.getIdString().toStdString()
                   + " dlc=" + std::to_string(msg.getLength())
                   + " data=" + msg.getDataHexString().toStdString() + ">";
        });

    m.def("make_lin_message", [](uint8_t id, uint8_t dlc) -> BusMessage
    {
        BusMessage msg(id);
        msg.setBusType(BusType::LIN);
        msg.setLength(dlc);
        return msg;
    }, py::arg("id"), py::arg("dlc") = 0);

    // --- send / receive ---

    m.def("lin_sleep", [](uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
            intf->sendLinSleepWakeup(false);
    }, py::arg("interface_id") = 0);

    m.def("lin_wakeup", [](uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
            intf->sendLinSleepWakeup(true);
    }, py::arg("interface_id") = 0);

    m.def("lin_set_schedule_table", [](uint8_t table_index, uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
            intf->setLinScheduleTable(table_index);
    }, py::arg("table_index"), py::arg("interface_id") = 0);

    m.def("send", [](BusMessage &msg, uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        msg.setInterfaceId(interface_id);
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
        {
            intf->sendMessage(msg);
        }
    }, py::arg("msg"), py::arg("interface_id") = 0);

    m.def("receive", [](double timeout_sec) -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        const unsigned long wait_ms = static_cast<unsigned long>(timeout_sec * 1000);

        {
            // Destructor order is LIFO: lck destructs first (releases QMutex),
            // then release destructs (re-acquires GIL). This ordering is required:
            // the GIL must be released before acquiring any Qt mutex to prevent a
            // deadlock where the CanListener thread holds _msgQueueMutex while the
            // main thread waits for the GIL.
            py::gil_scoped_release release;
            QMutexLocker lck(&g_activeEngine->msgQueueMutex());
            QQueue<BusMessage> &q = g_activeEngine->msgQueue();

            if (q.isEmpty() && !g_activeEngine->stopRequested())
            {
                g_activeEngine->msgQueueCondition().wait(
                    &g_activeEngine->msgQueueMutex(), wait_ms);
            }
        }

        {
            QMutexLocker lck(&g_activeEngine->msgQueueMutex());
            QQueue<BusMessage> &q = g_activeEngine->msgQueue();
            while (!q.isEmpty())
            {
                result.append(q.dequeue());
            }
        }

        return result;
    }, py::arg("timeout") = 1.0);

    // --- RX filter (applied before messages enter the receive() queue) ---

    m.def("set_filter", [](uint32_t id, uint32_t mask, py::object extended)
    {
        if (!g_activeEngine) { return; }
        std::optional<bool> ext;
        if (!extended.is_none())
        {
            ext = extended.cast<bool>();
        }
        g_activeEngine->setRxFilter(id, mask, ext);
    },
    py::arg("id"),
    py::arg("mask")     = 0xFFFFFFFFu,
    py::arg("extended") = py::none());

    m.def("clear_filter", []()
    {
        if (g_activeEngine) { g_activeEngine->clearRxFilter(); }
    });

    // By default TX frames (echo-back of sent messages) are excluded from receive().
    // Call enable_tx_echo(True) to include them.
    m.def("enable_tx_echo", [](bool enabled)
    {
        if (g_activeEngine) { g_activeEngine->setTxEchoEnabled(enabled); }
    }, py::arg("enabled") = true);

    // --- Periodic TX ---

    m.def("send_periodic", [](BusMessage msg, unsigned interval_ms, uint16_t interface_id) -> int
    {
        if (!g_activeEngine) { return -1; }
        return g_activeEngine->startPeriodicTask(msg, interval_ms, interface_id);
    },
    py::arg("msg"),
    py::arg("interval_ms"),
    py::arg("interface_id") = 0);

    m.def("stop_periodic", [](int handle)
    {
        if (g_activeEngine) { g_activeEngine->stopPeriodicTask(handle); }
    }, py::arg("handle"));

    // --- Trace access ---

    m.def("get_trace", [](int count) -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        // getSnapshot holds the BusTrace mutex for the entire copy, avoiding a
        // TOCTOU race between size() and getMessage() calls.
        QVector<BusMessage> snapshot;
        {
            py::gil_scoped_release release;
            snapshot = g_activeEngine->backend().getTrace()->getSnapshot(count);
        }

        for (const BusMessage &msg : std::as_const(snapshot))
        {
            result.append(msg);
        }
        return result;
    }, py::arg("count") = 0);

    m.def("trace_size", []() -> int
    {
        if (!g_activeEngine) { return 0; }
        return static_cast<int>(g_activeEngine->backend().getTrace()->size());
    });

    m.def("clear_trace", []()
    {
        if (!g_activeEngine) { return; }
        Backend &backend = g_activeEngine->backend();
        py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
        QMetaObject::invokeMethod(&backend, [&backend]()
        {
            backend.clearTrace();
        }, Qt::BlockingQueuedConnection);
    });

    m.def("save_trace", [](const std::string &path, py::object format)
    {
        if (!g_activeEngine)
        {
            throw std::runtime_error("no active engine");
        }

        const QString filename = QString::fromStdString(path);

        // An explicit format wins; otherwise infer from the extension. Unlike the
        // GUI save dialog, an unrecognised name is an error rather than a silent
        // fallback to ASC -- a script that writes the wrong format is worse than
        // one that stops.
        std::optional<TraceFileFormat> resolved;
        if (format.is_none())
        {
            resolved = traceFormatFromPath(filename);
            if (!resolved)
            {
                throw py::value_error(
                    "cannot infer trace format from '" + path
                    + "'; pass format=... (one of "
                    + supportedTraceFormatNames().join(", ").toStdString() + ")");
            }
        }
        else
        {
            const std::string name = format.cast<std::string>();
            resolved = traceFormatFromName(QString::fromStdString(name));
            if (!resolved)
            {
                throw py::value_error(
                    "unknown trace format '" + name + "'; expected one of "
                    + supportedTraceFormatNames().join(", ").toStdString());
            }
        }

        BusTrace *trace = g_activeEngine->backend().getTrace();

        // The writers take BusTrace's own mutex, so this is safe to call while a
        // measurement is running. Nothing below touches Python objects, so drop
        // the GIL for what may be a large amount of file IO.
        py::gil_scoped_release release;

        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            py::gil_scoped_acquire acquire;
            throw std::runtime_error("cannot open '" + path + "' for writing: "
                                     + file.errorString().toStdString());
        }

        trace->save(file, *resolved);
        file.close();

        if (file.error() != QFileDevice::NoError)
        {
            py::gil_scoped_acquire acquire;
            throw std::runtime_error("error writing '" + path + "': "
                                     + file.errorString().toStdString());
        }
    }, py::arg("path"), py::arg("format") = py::none(),
       "Save the current trace to a file. The format is taken from `format` "
       "('candump', 'asc', 'mdf', 'pcap', 'pcapng') or inferred from the file "
       "extension. Raises ValueError for an unknown format and OSError-like "
       "RuntimeError if the file cannot be written.");

    // --- Measurement control ---

    m.def("measurement_running", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        return g_activeEngine->backend().isMeasurementRunning();
    });

    m.def("start_measurement", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        Backend &backend = g_activeEngine->backend();
        bool result = false;
        {
            py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
            QMetaObject::invokeMethod(&backend, [&backend, &result]()
            {
                result = backend.startMeasurement();
            }, Qt::BlockingQueuedConnection);
        }
        return result;
    });

    m.def("stop_measurement", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        Backend &backend = g_activeEngine->backend();
        bool result = false;
        {
            py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
            QMetaObject::invokeMethod(&backend, [&backend, &result]()
            {
                result = backend.stopMeasurement();
            }, Qt::BlockingQueuedConnection);
        }
        return result;
    });

    // --- Interface helpers ---

    m.def("interfaces", []() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }
        for (BusInterfaceId id : g_activeEngine->backend().getInterfaceList())
        {
            py::dict d;
            d["id"]   = id;
            d["name"] = g_activeEngine->backend().getInterfaceName(id).toStdString();
            BusInterface *intf = g_activeEngine->backend().getInterfaceById(id);
            d["bus_type"] = (intf && intf->busType() == BusType::LIN) ? "LIN" : "CAN";
            d["state"]    = intf ? intf->getStateText().toStdString() : "";
            result.append(d);
        }
        return result;
    });

    m.def("interface_name", [](uint16_t id) -> std::string
    {
        if (!g_activeEngine) { return ""; }
        return g_activeEngine->backend().getInterfaceName(id).toStdString();
    });

    m.def("interface_state", [](uint16_t interface_id) -> std::string
    {
        if (!g_activeEngine) { return ""; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        return intf ? intf->getStateText().toStdString() : "";
    }, py::arg("interface_id"));

    // --- Logging ---

    m.def("log", [](const std::string &text)
    {
        if (g_activeEngine) { log_info(QString::fromStdString(text)); }
    });

    m.def("log_info", [](const std::string &text)
    {
        if (g_activeEngine) { log_info(QString::fromStdString(text)); }
    });

    m.def("log_warning", [](const std::string &text)
    {
        if (g_activeEngine) { log_warning(QString::fromStdString(text)); }
    });

    m.def("log_error", [](const std::string &text)
    {
        if (g_activeEngine) { log_error(QString::fromStdString(text)); }
    });

    // --- DBC / database access ---

    m.def("databases", []() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pCanDb &db : std::as_const(net->_canDbs))
            {
                py::dict dbInfo;
                dbInfo["file"]    = db->getFileName().toStdString();
                dbInfo["path"]    = db->getPath().toStdString();
                dbInfo["network"] = net->name().toStdString();

                py::list msgs;
                for (CanDbMessage *dbMsg : db->getMessageList())
                {
                    py::dict mInfo;
                    mInfo["name"] = dbMsg->getName().toStdString();
                    mInfo["id"]   = dbMsg->getRaw_id();
                    mInfo["dlc"]  = dbMsg->getDlc();

                    py::list sigNames;
                    for (CanDbSignal *sig : dbMsg->getSignals())
                    {
                        sigNames.append(sig->name().toStdString());
                    }
                    mInfo["signals"] = sigNames;
                    msgs.append(mInfo);
                }
                dbInfo["messages"] = msgs;
                result.append(dbInfo);
            }
        }
        return result;
    });

    // Decode signals from a received message using loaded DBCs.
    // Returns { "message": str, "id": int, "signals": { name: { "value", "raw", "unit", "min", "max" } } }
    // or None if no DBC definition is found.
    m.def("decode", [](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }

        py::dict sigDict;
        for (CanDbSignal *sig : dbMsg->getSignals())
        {
            if (!sig->isPresentInMessage(msg)) { continue; }

            const uint64_t raw  = sig->extractRawDataFromMessage(msg);
            const double   phys = sig->convertRawValueToPhysical(raw);

            py::dict sigInfo;
            sigInfo["value"] = phys;
            sigInfo["raw"]   = raw;
            sigInfo["unit"]  = sig->getUnit().toStdString();
            sigInfo["min"]   = sig->getMinimumValue();
            sigInfo["max"]   = sig->getMaximumValue();

            const QString valueName = sig->getValueName(raw);
            if (!valueName.isEmpty())
            {
                sigInfo["value_name"] = valueName.toStdString();
            }

            sigDict[py::cast(sig->name().toStdString())] = sigInfo;
        }

        py::dict result;
        result["message"] = dbMsg->getName().toStdString();
        result["id"]      = dbMsg->getRaw_id();
        result["signals"] = sigDict;

        CanDbNode *sender = dbMsg->getSender();
        if (sender) { result["sender"] = sender->name().toStdString(); }

        return result;
    }, py::arg("msg"));

    // Look up the DBC signal layout for a message (by live BusMessage).
    m.def("lookup", [](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }
        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }
        return buildMessageDict(dbMsg);
    }, py::arg("msg"));

    // Find a DBC message definition by name (str) or raw ID (int).
    // Returns the same dict as lookup(), or None.
    m.def("find_message", [](py::object name_or_id) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = nullptr;

        if (py::isinstance<py::str>(name_or_id))
        {
            const QString qname = QString::fromStdString(name_or_id.cast<std::string>());
            dbMsg = findDbMessageByName(g_activeEngine->backend(), qname);
        }
        else
        {
            // Numeric ID — build a dummy BusMessage with that raw_id
            BusMessage dummy;
            dummy.setRawId(name_or_id.cast<uint32_t>());
            dbMsg = g_activeEngine->backend().findDbMessage(dummy);
        }

        if (!dbMsg) { return py::none(); }
        return buildMessageDict(dbMsg);
    }, py::arg("name_or_id"));

    // Convenience: extract a single named signal's physical value from a message.
    // Returns float, or None if the message or signal is not found in the DBC.
    m.def("signal_value", [](const BusMessage &msg, const std::string &signal_name) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }

        CanDbSignal *sig = dbMsg->getSignalByName(QString::fromStdString(signal_name));
        if (!sig) { return py::none(); }

        return py::cast(sig->extractPhysicalFromMessage(msg));
    }, py::arg("msg"), py::arg("signal_name"));

    // Build a BusMessage by encoding signal physical values according to the DBC.
    // name_or_id: str (message name) or int (raw CAN ID)
    // signals:    dict mapping signal name -> physical value
    // Returns the populated Message, or raises ValueError on error.
    m.def("encode", [](py::object name_or_id, py::dict values) -> BusMessage
    {
        if (!g_activeEngine)
        {
            throw std::runtime_error("no active engine");
        }

        CanDbMessage *dbMsg = nullptr;
        if (py::isinstance<py::str>(name_or_id))
        {
            const QString qname = QString::fromStdString(name_or_id.cast<std::string>());
            dbMsg = findDbMessageByName(g_activeEngine->backend(), qname);
        }
        else
        {
            BusMessage dummy;
            dummy.setRawId(name_or_id.cast<uint32_t>());
            dbMsg = g_activeEngine->backend().findDbMessage(dummy);
        }

        if (!dbMsg)
        {
            throw py::value_error("message not found in loaded DBC databases");
        }

        // Create a zero-initialised message with the DBC id and DLC
        BusMessage msg;
        msg.setRawId(dbMsg->getRaw_id());
        msg.setLength(dbMsg->getDlc());

        for (auto item : values)
        {
            const QString sigName = QString::fromStdString(item.first.cast<std::string>());
            CanDbSignal *sig = dbMsg->getSignalByName(sigName);
            if (!sig)
            {
                throw py::value_error(
                    std::string("signal not found in DBC: ") + sigName.toStdString());
            }

            const double phys   = item.second.cast<double>();
            const double factor = sig->getFactor();
            const double offset = sig->getOffset();

            // Convert physical → raw
            if (factor == 0.0)
            {
                throw py::value_error(
                    std::string("signal '") + sigName.toStdString()
                    + "' has factor=0 in DBC — cannot encode");
            }

            uint64_t raw = 0;
            {
                const double rawD = (phys - offset) / factor;
                if (sig->isUnsigned())
                {
                    raw = static_cast<uint64_t>(rawD < 0.0 ? 0.0 : rawD + 0.5);
                    const uint64_t maxRaw = (sig->length() < 64)
                                           ? ((1ULL << sig->length()) - 1)
                                           : ~0ULL;
                    if (raw > maxRaw) { raw = maxRaw; }
                }
                else
                {
                    const uint8_t sigLen = sig->length();
                    const int64_t minRaw = (sigLen > 0 && sigLen < 64) ? -(1LL << (sigLen - 1)) : INT64_MIN;
                    const int64_t maxRaw = (sigLen > 0 && sigLen < 64) ? ((1LL << (sigLen - 1)) - 1) : INT64_MAX;
                    const int64_t s = std::clamp(
                        static_cast<int64_t>(rawD < 0.0 ? rawD - 0.5 : rawD + 0.5),
                        minRaw, maxRaw);
                    raw = static_cast<uint64_t>(s);
                }
            }

            msg.injectRawSignal(sig->startBit(), sig->length(), sig->isBigEndian(), raw);
        }

        return msg;
    }, py::arg("name_or_id"), py::arg("values"));

    // --- LIN database access ---

    // Build a signal-info dict for a LinSignal
    auto buildLinSignalDict = [](LinSignal *sig) -> py::dict
    {
        py::dict s;
        s["name"]       = sig->name().toStdString();
        s["bit_offset"] = sig->bitOffset();
        s["bit_length"] = sig->bitLength();
        s["factor"]     = sig->factor();
        s["offset"]     = sig->offset();
        s["min"]        = sig->minValue();
        s["max"]        = sig->maxValue();
        s["unit"]       = sig->unit().toStdString();
        s["publisher"]  = sig->publisher().toStdString();
        s["init_value"] = sig->initValue();
        return s;
    };

    // Build a frame-info dict for a LinFrame
    auto buildLinFrameDict = [&buildLinSignalDict](LinFrame *frame) -> py::dict
    {
        py::dict d;
        d["id"]        = frame->id();
        d["name"]      = frame->name().toStdString();
        d["publisher"] = frame->publisher().toStdString();
        d["length"]    = frame->length();

        py::list sigs;
        for (LinSignal *sig : frame->signalList())
        {
            sigs.append(buildLinSignalDict(sig));
        }
        d["signals"] = sigs;
        return d;
    };

    // List all loaded LDF databases with their frames.
    m.def("lin_databases", [buildLinFrameDict]() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pLinDb &db : std::as_const(net->_linDbs))
            {
                py::dict dbInfo;
                dbInfo["file"]    = db->fileName().toStdString();
                dbInfo["path"]    = db->path().toStdString();
                dbInfo["network"] = net->name().toStdString();
                dbInfo["speed"]   = db->speedBps();
                dbInfo["master"]  = db->masterNode().toStdString();

                py::list frames;
                for (LinFrame *frame : db->frames())
                {
                    frames.append(buildLinFrameDict(frame));
                }
                dbInfo["frames"] = frames;
                result.append(dbInfo);
            }
        }
        return result;
    });

    // Look up a LIN frame definition by name (str) or ID (int).
    // Returns a frame-info dict, or None if not found.
    m.def("find_lin_frame", [buildLinFrameDict](py::object name_or_id) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pLinDb &db : std::as_const(net->_linDbs))
            {
                LinFrame *frame = nullptr;
                if (py::isinstance<py::str>(name_or_id))
                {
                    frame = db->frameByName(QString::fromStdString(name_or_id.cast<std::string>()));
                }
                else
                {
                    frame = db->frameById(static_cast<uint8_t>(name_or_id.cast<int>()));
                }
                if (frame) { return buildLinFrameDict(frame); }
            }
        }
        return py::none();
    }, py::arg("name_or_id"));

    // Decode a received LIN BusMessage using loaded LDF databases.
    // Returns { "frame": str, "id": int, "signals": { name: { "value", "raw", "unit" } } }
    // or None if no LDF definition is found.
    m.def("decode_lin", [buildLinFrameDict](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        LinFrame *frame = g_activeEngine->backend().findLinFrame(msg);
        if (!frame) { return py::none(); }

        const uint8_t *data = msg.getData();
        const uint8_t  len  = msg.getLength();
        std::span<const uint8_t> payload{data, len};

        py::dict sigDict;
        for (LinSignal *sig : frame->signalList())
        {
            const uint64_t raw  = sig->extractRawValue(payload);
            const double   phys = sig->convertToPhysical(raw);

            py::dict sigInfo;
            sigInfo["value"]  = phys;
            sigInfo["raw"]    = raw;
            sigInfo["unit"]   = sig->unit().toStdString();

            const QString valueName = sig->getValueName(raw);
            if (!valueName.isEmpty())
            {
                sigInfo["value_name"] = valueName.toStdString();
            }

            sigDict[py::cast(sig->name().toStdString())] = sigInfo;
        }

        py::dict result;
        result["frame"]   = frame->name().toStdString();
        result["id"]      = frame->id();
        result["signals"] = sigDict;
        return result;
    }, py::arg("msg"));

    m.def("e2e_p2_compute_crc",
        [](const BusMessage &msg, uint16_t dataID) -> uint8_t
        {
            return e2e_p2_compute_crc(msg, dataID);
        },
        py::arg("msg"), py::arg("data_id"),
        "Compute AUTOSAR E2E Profile 2 CRC-8H2F over msg. "
        "Byte 0 is treated as 0x00; byte 1 must already hold the counter nibble. "
        "Returns the CRC byte without modifying the message.");

    m.def("e2e_p2_protect",
        [](BusMessage &msg, uint16_t dataID, uint8_t counter)
        {
            msg.setByte(1, counter & 0x0Fu);
            msg.setByte(0, e2e_p2_compute_crc(msg, dataID));
        },
        py::arg("msg"), py::arg("data_id"), py::arg("counter"),
        "Write AUTOSAR E2E Profile 2 header into msg in-place: "
        "sets counter nibble in byte 1, then computes and writes CRC into byte 0.");
}


// ---------------------------------------------------------------------------
// PythonEngine implementation
// ---------------------------------------------------------------------------

// Locates a Python standard library shipped alongside the executable and
// returns the prefix to use as PYTHONHOME, or an empty string if none is
// bundled. Checks both the portable layout used on Windows (<app>/lib/pythonX.Y,
// next to the .exe) and the FHS layout used inside an AppImage, where the
// binary lives in <prefix>/bin and the stdlib in <prefix>/lib/pythonX.Y.
[[nodiscard]] static QString findBundledPythonHome()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString landmark = QString("lib/python%1.%2/os.py")
                                 .arg(PY_MAJOR_VERSION)
                                 .arg(PY_MINOR_VERSION);

    for (const QString &prefix : { appDir, QDir::cleanPath(appDir + "/..") })
    {
        if (QFileInfo::exists(QDir(prefix).filePath(landmark)))
        {
            return prefix;
        }
    }

    return {};
}

static QString findPythonHome()
{
    if (const QString bundled = findBundledPythonHome(); !bundled.isEmpty())
    {
        return bundled;
    }

#ifndef Q_OS_WIN
    // On Unix the libpython we link against carries a usable compiled-in
    // prefix for regular (distro/package-manager) installs, so guessing a
    // PYTHONHOME from PATH would only risk overriding a correct one.
    return {};
#else
    for (const char *name : {"python3", "python"})
    {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (exe.isEmpty()) { continue; }

        QDir dir = QFileInfo(exe).absoluteDir();
        if (dir.dirName().compare("bin", Qt::CaseInsensitive) == 0 ||
            dir.dirName().compare("Scripts", Qt::CaseInsensitive) == 0)
        {
            dir.cdUp();
        }
        return dir.absolutePath();
    }

    return {};
#endif // Q_OS_WIN
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
struct PythonEngine::PyInterpreterHolder
{
    bool _env{ prepareEnvironment() };
    py::scoped_interpreter guard{};
    PyThreadState *savedState = nullptr;

    static bool prepareEnvironment()
    {
        if (qEnvironmentVariableIsEmpty("PYTHONHOME"))
        {
            const QString home = findPythonHome();
            if (!home.isEmpty())
            {
                qputenv("PYTHONHOME", home.toLocal8Bit());
            }
        }
        return true;
    }

    PyInterpreterHolder()
    {
        savedState = PyEval_SaveThread();
    }

    ~PyInterpreterHolder()
    {
        PyEval_RestoreThread(savedState);
    }
};
#pragma GCC diagnostic pop

PythonEngine::PythonEngine(Backend &backend, QObject *parent)
    : QObject(parent)
    , _backend(backend)
{
    try
    {
        _pyInterp = std::make_unique<PyInterpreterHolder>();
    }
    catch (const std::exception &e)
    {
        _initError = QString::fromStdString(e.what());
    }
}

PythonEngine::~PythonEngine()
{
    stopScript();
}

void PythonEngine::runScript(const QString &code)
{
    if (!_pyInterp)
    {
        emit scriptError(_initError.isEmpty()
            ? "Python interpreter failed to initialize."
            : _initError);
        return;
    }

    if (_running) { return; }

    _stopRequested = false;
    _running = true;

    {
        QMutexLocker lck(&_msgQueueMutex);
        _msgQueue.clear();
    }
    {
        QMutexLocker lck(&_inputQueueMutex);
        _inputQueue.clear();
    }

    emit scriptStarted();

    if (_workerThread && _workerThread->joinable())
    {
        _workerThread->join();
    }

    _workerThread = std::make_unique<std::thread>(&PythonEngine::workerFunc, this, code.toStdString());
}

void PythonEngine::stopScript()
{
    _stopRequested = true;
    _msgQueueCondition.wakeAll();
    _inputQueueCondition.wakeAll();

    stopAllPeriodicTasks();

    if (!_workerThread || !_workerThread->joinable()) { return; }

    for (int i = 0; i < 50 && _running; i++)
    {
        _msgQueueCondition.wakeAll();
        _inputQueueCondition.wakeAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (_workerThread->joinable())
    {
        if (_running)
        {
            // Script did not stop within the timeout. The worker thread still holds
            // 'this' via member captures and will keep emitting signals via 'this'
            // until it naturally exits, so we must not let 'this' be destroyed
            // before then. Null g_activeEngine so any subsequent pybind11 module
            // API calls inside the still-running thread return early, give up our
            // QObject parent (so our owner's destructor won't delete us too), and
            // hand the actual std::thread off to a watcher that joins it and only
            // then schedules our deletion.
            g_activeEngine = nullptr;
            setParent(nullptr);
            std::thread(&PythonEngine::waitForOrphanedThreadAndSelfDestruct, this, std::move(*_workerThread)).detach();
        }
        else
        {
            _workerThread->join();
        }
    }
    _workerThread.reset();
}

void PythonEngine::waitForOrphanedThreadAndSelfDestruct(std::thread worker)
{
    if (worker.joinable())
    {
        worker.join();
    }
    deleteLater();
}

bool PythonEngine::isRunning() const
{
    return _running;
}

void PythonEngine::enqueueMessage(const BusMessage &msg)
{
    if (!_running) { return; }
    if (msg.busType() == BusType::CAN && !msg.isRX() && !_echoTxEnabled.load()) { return; }
    if (!passesRxFilter(msg)) { return; }

    QMutexLocker lck(&_msgQueueMutex);
    if (_msgQueue.size() < 10000)
    {
        _msgQueue.enqueue(msg);
        _msgQueueCondition.wakeOne();
    }
}

void PythonEngine::enqueueInput(const QString &text)
{
    if (!_running) { return; }

    QMutexLocker lck(&_inputQueueMutex);
    _inputQueue.enqueue(text.endsWith('\n') ? text : text + '\n');
    _inputQueueCondition.wakeOne();
}

std::string PythonEngine::readInputLine()
{
    QMutexLocker lck(&_inputQueueMutex);
    while (_inputQueue.isEmpty() && !_stopRequested.load())
    {
        _inputQueueCondition.wait(&_inputQueueMutex);
    }

    if (_stopRequested.load()) { return {}; }
    return _inputQueue.dequeue().toUtf8().toStdString();
}

// ---------------------------------------------------------------------------
// RX filter
// ---------------------------------------------------------------------------

void PythonEngine::setRxFilter(uint32_t id, uint32_t mask, std::optional<bool> extended)
{
    QMutexLocker lck(&_rxFilterMutex);
    _rxFilter.id       = id;
    _rxFilter.mask     = mask;
    _rxFilter.extended = extended;
    _rxFilter.active   = true;
}

void PythonEngine::clearRxFilter()
{
    QMutexLocker lck(&_rxFilterMutex);
    _rxFilter.active = false;
}

bool PythonEngine::passesRxFilter(const BusMessage &msg) const
{
    QMutexLocker lck(&_rxFilterMutex);
    if (!_rxFilter.active) { return true; }
    if ((msg.getId() & _rxFilter.mask) != (_rxFilter.id & _rxFilter.mask)) { return false; }
    if (_rxFilter.extended.has_value() && msg.isExtended() != *_rxFilter.extended) { return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Periodic TX tasks
// ---------------------------------------------------------------------------

int PythonEngine::startPeriodicTask(BusMessage msg, unsigned interval_ms, uint16_t interface_id)
{
    // Allocate handle before taking the lock — only ever called from the Python
    // worker thread, so no concurrent access to _nextHandle.
    const int handle = _nextHandle++;
    auto task = std::make_shared<PeriodicTask>();

    // Construct the thread outside the lock so we don't hold _periodicMutex
    // during thread creation (which may briefly block on OS resources).
    task->thread = std::thread([this, msg, interval_ms, interface_id,
                                task_ptr = std::weak_ptr<PeriodicTask>(task)]() mutable
    {
        while (true)
        {
            auto task = task_ptr.lock();
            if (!task || task->stop.load() || _stopRequested.load()) { break; }

            BusInterface *intf = _backend.getInterfaceById(interface_id);
            if (intf)
            {
                BusMessage tx = msg;
                tx.setInterfaceId(interface_id);
                intf->sendMessage(tx);
            }

            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(interval_ms);
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto t = task_ptr.lock();
                if (!t || t->stop.load() || _stopRequested.load()) { return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    QMutexLocker lck(&_periodicMutex);
    _periodicTasks.emplace(handle, std::move(task));
    return handle;
}

void PythonEngine::stopPeriodicTask(int handle)
{
    std::shared_ptr<PeriodicTask> task;
    {
        QMutexLocker lck(&_periodicMutex);
        auto it = _periodicTasks.find(handle);
        if (it == _periodicTasks.end()) { return; }
        task = it->second;
        _periodicTasks.erase(it);
    }

    task->stop = true;
    if (task->thread.joinable()) { task->thread.join(); }
}

void PythonEngine::stopAllPeriodicTasks()
{
    std::map<int, std::shared_ptr<PeriodicTask>> tasks;
    {
        QMutexLocker lck(&_periodicMutex);
        tasks.swap(_periodicTasks);
    }

    for (auto &[handle, task] : tasks)
    {
        task->stop = true;
    }
    for (auto &[handle, task] : tasks)
    {
        if (task->thread.joinable()) { task->thread.join(); }
    }
}

// ---------------------------------------------------------------------------
// Script worker
// ---------------------------------------------------------------------------

void PythonEngine::workerFunc(std::string code)
{
    g_activeEngine = this;

    PyGILState_STATE gstate = PyGILState_Ensure();

    try
    {
        auto globals = py::globals();

        globals["_cangaroo_output"] = py::cpp_function(
            [this](const std::string &text, bool is_err)
            {
                const QString qtext = QString::fromStdString(text);
                if (is_err) { emit scriptError(qtext); }
                else        { emit scriptOutput(qtext); }
            });

        globals["_cangaroo_stop_check"] = py::cpp_function(
            [this]() -> bool { return _stopRequested.load(); });

        globals["_cangaroo_input"] = py::cpp_function(
            [this]() -> std::string { return readInputLine(); },
            py::call_guard<py::gil_scoped_release>());

        py::exec(R"(
import io
import sys

class _SignalWriter:
    def __init__(self, is_err):
        self._is_err = is_err
    def write(self, text):
        if text:
            _cangaroo_output(text, self._is_err)
    def flush(self):
        pass

sys.stdout = _SignalWriter(False)
sys.stderr = _SignalWriter(True)

class _SignalReader(io.TextIOBase):
    def __init__(self):
        super().__init__()
        self._buffer = ""
    def readline(self, size=-1):
        if not self._buffer:
            self._buffer = _cangaroo_input()
            if not self._buffer and _cangaroo_stop_check():
                raise KeyboardInterrupt("Script stopped by user")
        if size is None or size < 0:
            line, self._buffer = self._buffer, ""
            return line
        line, self._buffer = self._buffer[:size], self._buffer[size:]
        return line
    def readable(self):
        return True
    def isatty(self):
        return True

sys.stdin = _SignalReader()
)");

        py::exec(R"(
import threading as _threading

def _cangaroo_trace(frame, event, arg):
    if _cangaroo_stop_check():
        raise KeyboardInterrupt("Script stopped by user")
    return _cangaroo_trace

sys.settrace(_cangaroo_trace)
_threading.settrace(_cangaroo_trace)
)");

        try
        {
            py::exec(code);
        }
        catch (py::error_already_set &e)
        {
            const QString err = QString::fromStdString(e.what());
            if (!err.contains("KeyboardInterrupt"))
            {
                emit scriptError(err + "\n");
            }
        }
    }
    catch (std::exception &e)
    {
        emit scriptError(QString::fromStdString(e.what()) + "\n");
    }

    PyGILState_Release(gstate);

    g_activeEngine = nullptr;
    _running = false;
    emit scriptFinished();
}
