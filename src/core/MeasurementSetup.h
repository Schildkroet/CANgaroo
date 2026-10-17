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

#pragma once

#include <QObject>
#include <QList>
#include <QHash>
#include <QDomDocument>

class Backend;
class MeasurementNetwork;
class BusTrace;
class BusInterface;
class CanDbMessage;
class LinFrame;
class BusMessage;

class MeasurementSetup : public QObject
{
    Q_OBJECT

public:
    explicit MeasurementSetup(QObject *parent);
    virtual ~MeasurementSetup();
    void clear();

    CanDbMessage *findDbMessage(const BusMessage &msg) const;
    LinFrame     *findLinFrame(const BusMessage &msg) const;
    void rebuildMessageCache();
    QString getInterfaceName(const BusInterface &id) const;

    // Database reload coordination. Call beginDatabaseReload() before mutating any
    // loaded CanDb/LinDb so that consumers (e.g. the GraphWindow decoder thread)
    // release their cached signal/frame pointers; call endDatabaseReload() afterwards
    // to rebuild the message/frame caches and refresh all consumers.
    void beginDatabaseReload();
    void endDatabaseReload();

    int countNetworks() const;
    MeasurementNetwork *getNetwork(int index) const;
    MeasurementNetwork *getNetworkByName(QString name) const;
    QList<MeasurementNetwork*> getNetworks();
    MeasurementNetwork *createNetwork();
    void removeNetwork(MeasurementNetwork *network);

    void cloneFrom(MeasurementSetup &origin);
    bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);
    bool loadXML(Backend &backend, QDomElement &el);

signals:
    void onSetupChanged();

private:
    QList<MeasurementNetwork*> _networks;
    mutable QHash<uint32_t, CanDbMessage*> _messageCache;
    mutable QHash<uint32_t, CanDbMessage*> _pgnCache; // J1939 databases, extended messages only
    mutable QHash<uint8_t,  LinFrame*>     _linFrameCache;
};
