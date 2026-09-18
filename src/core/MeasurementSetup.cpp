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

#include "MeasurementSetup.h"

#include <QThread>
#include <QMetaType>

#include "core/BusTrace.h"
#include "core/BusMessage.h"
#include "core/MeasurementNetwork.h"
#include "core/DBC/LinDb.h"
#include "core/DBC/LinFrame.h"

MeasurementSetup::MeasurementSetup(QObject *parent)
  : QObject(parent)
{
}

MeasurementSetup::~MeasurementSetup()
{
    qDeleteAll(_networks);
}

void MeasurementSetup::clear()
{
    qDeleteAll(_networks);
    _networks.clear();
    _messageCache.clear();
    emit onSetupChanged();
}

void MeasurementSetup::cloneFrom(MeasurementSetup &origin)
{
    clear();
    for (auto *network : origin._networks) {
        MeasurementNetwork *network_copy = new MeasurementNetwork();
        network_copy->cloneFrom(*network);
        _networks.append(network_copy);
    }
    rebuildMessageCache();
    emit onSetupChanged();
}

bool MeasurementSetup::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    for (auto *network : _networks) {
        QDomElement networkNode = xml.createElement("network");
        if (!network->saveXML(backend, xml, networkNode)) {
            return false;
        }
        root.appendChild(networkNode);
    }
    return true;
}

bool MeasurementSetup::loadXML(Backend &backend, QDomElement &el)
{
    clear();

    QDomNodeList networks = el.elementsByTagName("network");
    for (int i=0; i<networks.length(); i++) {
        MeasurementNetwork *network = createNetwork();
        if (!network->loadXML(backend, networks.item(i).toElement())) {
            return false;
        }
    }

    rebuildMessageCache();
    emit onSetupChanged();
    return true;
}


MeasurementNetwork *MeasurementSetup::createNetwork()
{
    MeasurementNetwork *network = new MeasurementNetwork();
    _networks.append(network);
    return network;
}

void MeasurementSetup::removeNetwork(MeasurementNetwork *network)
{
    if (_networks.removeAll(network) > 0) {
        delete network;
    }
}


void MeasurementSetup::rebuildMessageCache()
{
    _messageCache.clear();
    _linFrameCache.clear();
    for (auto *network : _networks) {
        for (const auto &db : network->_canDbs) {
            const CanDbMessageList &msgs = db->getMessageList();
            for (auto it = msgs.constBegin(); it != msgs.constEnd(); ++it) {
                // DBC stores extended IDs with bit 31 set; strip it to match BusMessage::getRawId()
                _messageCache.insert(it.key() & 0x1FFFFFFF, it.value());
            }
        }
        for (const auto &lindb : network->_linDbs) {
            for (LinFrame *frame : lindb->frames()) {
                _linFrameCache.insert(frame->id(), frame);
            }
        }
    }
}

void MeasurementSetup::beginDatabaseReload()
{
    // Notify consumers before the DBs change. The GraphWindow handles this by
    // synchronously draining its decoder thread (clearActiveSignals via a
    // BlockingQueuedConnection), so no other thread reads the DB objects while
    // they are being mutated/replaced below.
    emit onSetupChanged();
}

void MeasurementSetup::endDatabaseReload()
{
    // Rebuild the raw-pointer caches (which may now reference new frames after a
    // LIN reload, or new messages after a DBC reload) before any consumer can use
    // them again, then refresh all consumers.
    rebuildMessageCache();
    emit onSetupChanged();
}

CanDbMessage *MeasurementSetup::findDbMessage(const BusMessage &msg) const
{
    if (msg.busType() != BusType::CAN) return nullptr;
    // Error frames carry no CAN ID (drivers set it to 0): never match a DBC entry.
    if (msg.isErrorFrame()) return nullptr;
    auto it = _messageCache.constFind(msg.getRawId());
    if (it != _messageCache.constEnd()) {
        return it.value();
    }
    return nullptr;
}

LinFrame *MeasurementSetup::findLinFrame(const BusMessage &msg) const
{
    if (msg.busType() != BusType::LIN) return nullptr;
    return _linFrameCache.value(static_cast<uint8_t>(msg.getId() & 0x3F), nullptr);
}

QString MeasurementSetup::getInterfaceName(const BusInterface &id) const
{
    return id.getName();
}

int MeasurementSetup::countNetworks() const
{
    return _networks.length();
}

MeasurementNetwork *MeasurementSetup::getNetwork(int index) const
{
    return _networks.value(index);
}

MeasurementNetwork *MeasurementSetup::getNetworkByName(QString name) const
{
    for (auto *network : _networks) {
        if (network->name() == name) {
            return network;
        }
    }
    return 0;
}

QList<MeasurementNetwork *> MeasurementSetup::getNetworks()
{
    return _networks;
}

