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

#include "BusMessage.h"
#include "CanDbMessage.h"
#include <QString>
#include <QMap>

class CanDbMessage;

using CanDbValueTable = QMap<uint64_t, QString>;

class CanDbSignal
{
public:
    CanDbSignal(CanDbMessage *parent);
    QString name() const;
    void setName(const QString &name);
    
    CanDbMessage* getParentMessage() const { return _parent; }

    uint16_t startBit() const;
    void setStartBit(uint16_t startBit);

    // startBit() is internal: for big-endian signals it is an MSB-first bit index.
    // These give the numbers as written in the DBC file and as shown by Vector tools.
    [[nodiscard]] uint16_t dbcStartBit() const;   // DBC "SG_ name : <start>|len@0"
    [[nodiscard]] uint16_t lsbBitIndex() const;   // Intel-style (byte*8 + bit) position of the LSB
    [[nodiscard]] QString startBitDescription() const;

    uint16_t length() const;
    void setLength(uint16_t length);

    QString comment() const;
    void setComment(const QString &comment);

    QString getValueName(const uint64_t value) const;
    void setValueName(const uint64_t value, const QString &name);

    double getFactor() const;
    void setFactor(double factor);

    double getOffset() const;
    void setOffset(double offset);

    double getMinimumValue() const;
    void setMinimumValue(double min);

    double getMaximumValue() const;
    void setMaximumValue(double max);

    QString getUnit() const;
    void setUnit(const QString &unit);

    bool isUnsigned() const;
    void setUnsigned(bool isUnsigned);

    bool isBigEndian() const;
    void setIsBigEndian(bool isBigEndian);

    bool isMuxer() const;
    void setIsMuxer(bool isMuxer);

    bool isMuxed() const;
    void setIsMuxed(bool isMuxed);

    uint32_t getMuxValue() const;
    void setMuxValue(const uint32_t &muxValue);

    bool isPresentInMessage(const BusMessage &msg) const;
    uint64_t extractRawDataFromMessage(const BusMessage &msg) const;

    double convertRawValueToPhysical(const uint64_t rawValue) const;
    double extractPhysicalFromMessage(const BusMessage &msg) const;

    void injectRawSignalIntoMessage(BusMessage &msg, uint64_t rawValue) const;
    void injectPhysicalIntoMessage(BusMessage &msg, double physicalValue) const;


private:
    CanDbMessage *_parent;
    QString _name;
    uint16_t _startBit;
    uint16_t _length;
    bool _isUnsigned;
    bool _isBigEndian;
    double _factor;
    double _offset;
    double _min;
    double _max;
    QString _unit;
    bool _isMuxer;
    bool _isMuxed;
    uint32_t _muxValue;
    QString _comment;
    CanDbValueTable _valueTable;
};
