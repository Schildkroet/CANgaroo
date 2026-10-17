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

#include <QString>
#include <QList>
#include <QMap>
#include <QSharedPointer>

#include "CanDbNode.h"
#include "CanDbMessage.h"

class QDomDocument;
class QDomElement;

class Backend;
class CanDb;
class CanDbMessage;

using CanDbNodeMap = QMap<QString, CanDbNode*>;
using CanDbMessageList = QMap<uint32_t, CanDbMessage*>;
using pCanDb = QSharedPointer<CanDb>;

class CanDb
{
    public:
        CanDb();

        void setPath(QString path) { _path = path; }
        QString getPath() { return _path; }
        QString getFileName();
        QString getDirectory();

        void setVersion(QString version) { _version = version; }
        QString getVersion() { return _version; }

        // Set by the DBC attribute BA_ "ProtocolType" "J1939"; messages of such a
        // database are matched by PGN instead of by full CAN identifier.
        void setJ1939(bool j1939) { _isJ1939 = j1939; }
        [[nodiscard]] bool isJ1939() const { return _isJ1939; }

        CanDbNode *getOrCreateNode(QString node_name);

        size_t getNumberOfMessages();

        CanDbMessage *getMessageById(uint32_t raw_id);
        const CanDbMessageList &getMessageList() const;

        void addMessage(CanDbMessage *msg);

        QString getComment() const;
        void setComment(const QString &comment);

        bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);

        void updateFrom(CanDb *other);

private:
        QString _path;
        QString _version;
        QString _comment;
        bool _isJ1939 = false;
        CanDbNodeMap _nodes;
        CanDbMessageList _messages;

};
