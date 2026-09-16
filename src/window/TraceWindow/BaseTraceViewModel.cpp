/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "BaseTraceViewModel.h"
#include "qtooltip.h"

#include <QDateTime>
#include <QLocale>
#include <QColor>
#include <QSettings>

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/BusMessage.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/LinSignal.h"
#include "core/ThemeManager.h"
#include <iostream>

BaseTraceViewModel::BaseTraceViewModel(Backend &backend)
{
    _backend = &backend;
    _dataAsciiMode = QSettings().value("tracewindow/dataAsciiMode", false).toBool();

    // Interface names are stable while a setup is active; drop cached names
    // whenever interfaces may be re-enumerated
    connect(&backend, &Backend::onSetupChanged, this, [this]() { _interfaceNameCache.clear(); });
    connect(&backend, &Backend::beginMeasurement, this, [this]() { _interfaceNameCache.clear(); });

    connect(&backend, &Backend::onDisplayConfigChanged, this, [this]() {
        setDataAsciiMode(QSettings().value("tracewindow/dataAsciiMode", false).toBool());
    });
}

QString BaseTraceViewModel::interfaceName(BusInterfaceId id) const
{
    auto it = _interfaceNameCache.constFind(id);
    if (it != _interfaceNameCache.constEnd()) {
        return it.value();
    }
    QString name = _backend->getInterfaceName(id);
    _interfaceNameCache.insert(id, name);
    return name;
}

int BaseTraceViewModel::columnCount(const QModelIndex &parent) const
{
    (void) parent;
    return column_count;
}

QVariant BaseTraceViewModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole) {

        if (orientation == Qt::Horizontal) {
            switch (section) {
                case column_index:
                return QString(tr("Index"));
                case column_timestamp:
                    return QString(tr("Time"));
                case column_channel:
                    return QString(tr("Channel"));
                case column_direction:
                    return QString(tr("RX/TX"));
                case column_type:
                    return QString(tr("Type"));
                case column_canid:
                    return QString("ID");
                case column_sender:
                    return QString(tr("Sender"));
                case column_name:
                    return QString(tr("Name"));
                case column_dlc:
                    return QString("DLC");
                case column_data:
                    return QString(tr("Data"));
                case column_comment:
                    return QString(tr("Comment"));
            }
        }

    }
    else if (role == Qt::TextAlignmentRole) {
        switch (section) {
            case column_index:     return QVariant(Qt::AlignCenter);
            case column_timestamp: return QVariant(Qt::AlignCenter);
            case column_channel:   return QVariant(Qt::AlignCenter);
            case column_direction: return QVariant(Qt::AlignCenter);
            case column_type:      return QVariant(Qt::AlignCenter);
            case column_canid:     return QVariant(Qt::AlignCenter);
            case column_sender:    return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
            case column_name:      return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
            case column_dlc:       return QVariant(Qt::AlignCenter);
            case column_data:      return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
            case column_comment:   return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
            default: return QVariant();
        }
    }
    return QVariant();
}


QVariant BaseTraceViewModel::data(const QModelIndex &index, int role) const
{
    switch (role) {
        case Qt::DisplayRole:
            return data_DisplayRole(index, role);
        case Qt::TextAlignmentRole:
            return data_TextAlignmentRole(index, role);
        case Qt::ForegroundRole:
            return data_TextColorRole(index, role);
        case ChangedBytesRole:
            return data_ChangedBytesRole(index);
        case Qt::ToolTipRole:
        {
            QString data = index.data(Qt::DisplayRole).toString();
            uint  length = data.length();
            if(length>24)
            {
                uint div = length / 24;
                for(uint i = 0;i< div-1;i++)
                {
                    if((i+1)%2 == 0 || index.column() != column_data)
                        data.insert(24*(i+1)+i,"\n");
                    else
                        data.insert(24*(i+1)+i," ");
                }
            }
            return data;
        }
            //return QString("Row%1, Column%2").arg(row + 1).arg(col +1);
        default:
            return QVariant();
    }
}

Backend *BaseTraceViewModel::backend() const
{
    return _backend;
}

BusTrace *BaseTraceViewModel::trace() const
{
    return _backend->getTrace();
}

timestamp_mode_t BaseTraceViewModel::timestampMode() const
{
    return _timestampMode;
}

void BaseTraceViewModel::setTimestampMode(timestamp_mode_t timestampMode)
{
    _timestampMode = timestampMode;
}

bool BaseTraceViewModel::dataAsciiMode() const
{
    return _dataAsciiMode;
}

void BaseTraceViewModel::setDataAsciiMode(bool ascii)
{
    if (_dataAsciiMode == ascii) { return; }
    _dataAsciiMode = ascii;
    emit layoutChanged();
}

QVariant BaseTraceViewModel::formatTimestamp(timestamp_mode_t mode, const BusMessage &currentMsg, const BusMessage &lastMsg) const
{

    if (mode==timestamp_mode_delta) {

        double t_current = currentMsg.getFloatTimestamp();
        double t_last = lastMsg.getFloatTimestamp();
        if (t_last==0) {
            return QString::number(0.0, 'f', 3);
        } else {
            return QString::number(qMax(0.0, t_current - t_last), 'f', 3);
        }

    } else if (mode==timestamp_mode_absolute) {

        return QLocale::c().toString(currentMsg.getDateTime(), "hh:mm:ss.zzz");

    } else if (mode==timestamp_mode_absolute_utc) {

        return QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(currentMsg.getTimestamp_ms(), Qt::UTC), "hh:mm:ss.zzz");

    } else if (mode==timestamp_mode_relative) {

        double t_current = currentMsg.getFloatTimestamp();
        return QString::number(t_current - backend()->getTimestampAtMeasurementStart(), 'f', 3);

    }

    return QVariant();
}

QVariant BaseTraceViewModel::data_DisplayRole(const QModelIndex &index, int role) const
{
    (void) index;
    (void) role;
    return QVariant();
}

QVariant BaseTraceViewModel::data_DisplayRole_Message(const QModelIndex &index, int role, const BusMessage &currentMsg, const BusMessage &lastMsg) const
{
    (void) role;

    const bool isLin = (currentMsg.busType() == BusType::LIN);

    switch (index.column()) {

        case column_index:
            return index.internalId();

        case column_timestamp:
            return formatTimestamp(_timestampMode, currentMsg, lastMsg);

        case column_channel:
            return interfaceName(currentMsg.getInterfaceId());

        case column_direction:
            return currentMsg.isRX() ? tr("RX") : tr("TX");

        case column_type:
        {
            if (isLin) {
                if (currentMsg.isLinSleepFrame())  return QStringLiteral("LIN.SLP");
                if (currentMsg.isLinWakeupFrame()) return QStringLiteral("LIN.WUP");
                return QStringLiteral("LIN");
            }
            if (currentMsg.isErrorFrame()) {
                return QStringLiteral("ERR");
            }
            QString _type = QString(currentMsg.isFD() ? "FD.":"") + QString(currentMsg.isExtended()? "EXT" : "STD") + QString(currentMsg.isRTR()?".RTR":"") + QString((currentMsg.isBRS()?".BRS":""));
            return _type;
        }

        case column_canid:
            // A CAN error frame carries error classes instead of an ID. A LIN
            // frame flagged as an error (no response, bad checksum) is a real
            // frame, so keep its ID visible.
            return (currentMsg.isErrorFrame() && currentMsg.busType() == BusType::CAN)
                       ? QStringLiteral("-")
                       : currentMsg.getIdString();

        case column_name:
        {
            if (isLin) {
                if (currentMsg.isLinSleepFrame())  return tr("Sleep");
                if (currentMsg.isLinWakeupFrame()) return tr("Wakeup");
                LinFrame *linFrame = backend()->findLinFrame(currentMsg);
                return linFrame ? linFrame->name() : QStringLiteral("");
            }
            CanDbMessage *dbmsg = backend()->findDbMessage(currentMsg);
            return (dbmsg) ? dbmsg->getName() : "";
        }

        case column_sender:
        {
            if (isLin) {
                LinFrame *linFrame = backend()->findLinFrame(currentMsg);
                return linFrame ? linFrame->publisher() : QStringLiteral("");
            }
            CanDbMessage *dbmsg = backend()->findDbMessage(currentMsg);
            return (dbmsg) ? dbmsg->getSender()->name() : "";
        }

        case column_dlc:
            return currentMsg.getLength();

        case column_data:
            return _dataAsciiMode ? currentMsg.getDataAsciiString() : currentMsg.getDataHexString();

        case column_comment:
        {
            if (isLin)
                return QStringLiteral("");
            CanDbMessage *dbmsg = backend()->findDbMessage(currentMsg);
            return (dbmsg) ? dbmsg->getComment() : "";
        }

        default:
            return QVariant();

    }
}

QVariant BaseTraceViewModel::data_DisplayRole_Signal(const QModelIndex &index, int role, const BusMessage &msg) const
{
    (void) role;

    if (msg.busType() == BusType::LIN) {
        LinFrame *linFrame = backend()->findLinFrame(msg);
        if (!linFrame) { return QVariant(); }

        const LinSignalList &sigs = linFrame->signalList();
        if (index.row() < 0 || index.row() >= sigs.size()) { return QVariant(); }
        const LinSignal *signal = sigs.at(index.row());

        switch (index.column()) {
            case column_name:
                return signal->name();
            case column_data:
            {
                uint64_t raw = signal->extractRawValue({msg.getData(), static_cast<std::size_t>(msg.getLength())});
                const QString valName = signal->getValueName(raw);
                if (valName.isEmpty()) {
                    const QString unit = signal->unit();
                    double phys = signal->convertToPhysical(raw);
                    return unit.isEmpty()
                        ? QVariant(phys)
                        : QVariant(QStringLiteral("%1 %2").arg(phys).arg(unit));
                }
                return QStringLiteral("%1 - %2").arg(raw).arg(valName);
            }
            default:
                return QVariant();
        }
    }

    uint64_t raw_data;
    QString value_name;
    QString unit;

    CanDbMessage *dbmsg = backend()->findDbMessage(msg);
    if (!dbmsg) { return QVariant(); }

    CanDbSignal *dbsignal = dbmsg->getSignal(index.row());
    if (!dbsignal) { return QVariant(); }

    switch (index.column()) {

        case column_name:
            return dbsignal->name();

        case column_data:

            if (dbsignal->isPresentInMessage(msg)) {
                raw_data = dbsignal->extractRawDataFromMessage(msg);
            } else {
                if (!trace()->getMuxedSignalFromCache(dbsignal, &raw_data)) {
                    return QVariant();
                }
            }

            value_name = dbsignal->getValueName(raw_data);
            if (value_name.isEmpty()) {
                unit = dbsignal->getUnit();
                if (unit.isEmpty()) {
                    return dbsignal->convertRawValueToPhysical(raw_data);
                } else {
                    return QString("%1 %2").arg(dbsignal->convertRawValueToPhysical(raw_data)).arg(unit);
                }
            } else {
                return QString("%1 - %2").arg(raw_data).arg(value_name);
            }

        case column_comment:
            return dbsignal->comment().replace('\n', ' ');

        default:
            return QVariant();

    }
}

QVariant BaseTraceViewModel::data_TextAlignmentRole(const QModelIndex &index, int role) const
{
    (void) role;
    switch (index.column()) {
        case column_index:     return QVariant(Qt::AlignCenter);
        case column_timestamp: return QVariant(Qt::AlignRight  | Qt::AlignVCenter);
        case column_channel:   return QVariant(Qt::AlignCenter);
        case column_direction: return QVariant(Qt::AlignCenter);
        case column_type:      return QVariant(Qt::AlignCenter);
        case column_canid:     return QVariant(Qt::AlignCenter);
        case column_sender:    return QVariant(Qt::AlignCenter);
        case column_name:      return QVariant(Qt::AlignLeft   | Qt::AlignVCenter);
        case column_dlc:       return QVariant(Qt::AlignCenter);
        case column_data:      return QVariant(Qt::AlignLeft   | Qt::AlignVCenter);
        case column_comment:   return QVariant(Qt::AlignLeft   | Qt::AlignVCenter);
        default: return QVariant();
    }
}

QVariant BaseTraceViewModel::data_TextColorRole(const QModelIndex &index, int role) const
{
    (void) index;
    (void) role;
    return QVariant();
}

QVariant BaseTraceViewModel::data_ChangedBytesRole(const QModelIndex &index) const
{
    (void) index;
    return QVariant();
}

QVariant BaseTraceViewModel::data_TextColorRole_Signal(const QModelIndex &index, int role, const BusMessage &msg) const
{
    (void) role;

    // LIN signals are always present in the frame (no multiplexing)
    if (msg.busType() == BusType::LIN)
        return QVariant();

    CanDbMessage *dbmsg = backend()->findDbMessage(msg);
    if (!dbmsg) { return QVariant(); }

    CanDbSignal *dbsignal = dbmsg->getSignal(index.row());
    if (!dbsignal) { return QVariant(); }

    if (dbsignal->isPresentInMessage(msg)) {
        return QVariant(); // default text color
    } else {
        bool isDark = ThemeManager::instance().isDarkMode();
        return QVariant::fromValue(isDark ? QColor(100, 100, 100) : QColor(200, 200, 200));
    }
}

