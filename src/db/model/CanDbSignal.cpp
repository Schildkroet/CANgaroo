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

#include "CanDbSignal.h"

#include <cmath>

CanDbSignal::CanDbSignal(CanDbMessage *parent)
  : _parent(parent),
    _startBit(0),
    _length(0),
    _isUnsigned(false),
    _isBigEndian(false),
    _factor(1),
    _offset(0),
    _min(0),
    _max(0),
    _isMuxer(false),
    _isMuxed(false),
    _muxValue(0)
{
}

QString CanDbSignal::name() const
{
    return _name;
}

void CanDbSignal::setName(const QString &name)
{
    _name = name;
}

uint16_t CanDbSignal::startBit() const
{
    return _startBit;
}

void CanDbSignal::setStartBit(uint16_t startBit)
{
    _startBit = startBit;
}

uint16_t CanDbSignal::dbcStartBit() const
{
    if (!isBigEndian()) { return _startBit; }

    // The parser conversion (byte*8 + (7 - bit)) is its own inverse.
    return static_cast<uint16_t>(((_startBit >> 3) * 8) + (7 - (_startBit & 0b111)));
}

uint16_t CanDbSignal::lsbBitIndex() const
{
    if (!isBigEndian()) { return _startBit; }

    const uint16_t last = static_cast<uint16_t>(_startBit + (_length > 0 ? _length - 1 : 0));
    return static_cast<uint16_t>(((last >> 3) * 8) + (7 - (last & 0b111)));
}

QString CanDbSignal::startBitDescription() const
{
    if (!isBigEndian())
    {
        return QObject::tr("Start bit %1 (little-endian, LSB), length %2").arg(_startBit).arg(_length);
    }

    return QObject::tr("Start bit %1 (big-endian, MSB, as in DBC), LSB at bit %2, length %3")
        .arg(dbcStartBit()).arg(lsbBitIndex()).arg(_length);
}

uint16_t CanDbSignal::length() const
{
    return _length;
}

void CanDbSignal::setLength(uint16_t length)
{
    _length = length;
}

QString CanDbSignal::comment() const
{
    return _comment;
}

void CanDbSignal::setComment(const QString &comment)
{
    _comment = comment;
}

QString CanDbSignal::getValueName(const uint64_t value) const
{
    if (_valueTable.contains(value)) {
        return _valueTable[value];
    } else {
        return QString();
    }
}

void CanDbSignal::setValueName(const uint64_t value, const QString &name)
{
    _valueTable[value] = name;
}

double CanDbSignal::convertRawValueToPhysical(const uint64_t rawValue) const
{
    // A length outside 1..64 bits cannot be sign-extended: the shift width would
    // be 64 (or negative), which is undefined behaviour. DBC files are not
    // validated on that field, so fall back to the unsigned reading.
    if (isUnsigned() || _length == 0 || _length > 64) {
        uint64_t v = rawValue;
        return v * _factor + _offset;
    } else {
        int64_t v = static_cast<int64_t>(rawValue << (64 - _length));
        v >>= (64 - _length);
        return v * _factor + _offset;
    }
}

double CanDbSignal::extractPhysicalFromMessage(const BusMessage &msg) const
{
    return convertRawValueToPhysical(extractRawDataFromMessage(msg));
}

double CanDbSignal::getFactor() const
{
    return _factor;
}

void CanDbSignal::setFactor(double factor)
{
    _factor = factor;
}

double CanDbSignal::getOffset() const
{
    return _offset;
}

void CanDbSignal::setOffset(double offset)
{
    _offset = offset;
}

double CanDbSignal::getMinimumValue() const
{
    return _min;
}

void CanDbSignal::setMinimumValue(double min)
{
    _min = min;
}

double CanDbSignal::getMaximumValue() const
{
    return _max;
}

void CanDbSignal::setMaximumValue(double max)
{
    _max = max;
}

QString CanDbSignal::getUnit() const
{
    return _unit;
}

void CanDbSignal::setUnit(const QString &unit)
{
    _unit = unit;
}

bool CanDbSignal::isUnsigned() const
{
    return _isUnsigned;
}

void CanDbSignal::setUnsigned(bool isUnsigned)
{
    _isUnsigned = isUnsigned;
}
bool CanDbSignal::isBigEndian() const
{
    return _isBigEndian;
}

void CanDbSignal::setIsBigEndian(bool isBigEndian)
{
    _isBigEndian = isBigEndian;
}

bool CanDbSignal::isMuxer() const
{
    return _isMuxer;
}

void CanDbSignal::setIsMuxer(bool isMuxer)
{
    _isMuxer = isMuxer;
}

bool CanDbSignal::isMuxed() const
{
    return _isMuxed;
}

void CanDbSignal::setIsMuxed(bool isMuxed)
{
    _isMuxed = isMuxed;
}

uint32_t CanDbSignal::getMuxValue() const
{
    return _muxValue;
}

void CanDbSignal::setMuxValue(const uint32_t &muxValue)
{
    _muxValue = muxValue;
}

bool CanDbSignal::isPresentInMessage(const BusMessage &msg) const
{
    if (msg.getRawId() != (_parent->getRaw_id() & 0x1FFFFFFF)) {
        return false;
    }

    if ((_startBit + _length)>(8*msg.getLength())) {
        return false;
    }

    if (!_isMuxed) { return true; }

    CanDbSignal *muxer = _parent->getMuxer();
    if (!muxer) { return false; }

    return _muxValue == muxer->extractRawDataFromMessage(msg);
}

uint64_t CanDbSignal::extractRawDataFromMessage(const BusMessage &msg) const
{
    return msg.extractRawSignal(startBit(), length(), isBigEndian());
}

void CanDbSignal::injectRawSignalIntoMessage(BusMessage &msg, uint64_t rawValue) const
{
    msg.injectRawSignal(startBit(), length(), isBigEndian(), rawValue);
}

void CanDbSignal::injectPhysicalIntoMessage(BusMessage &msg, double physicalValue) const
{
    double rawDouble = (physicalValue - _offset) / _factor;
    uint64_t rawValue;
    if (_isUnsigned) {
        rawValue = static_cast<uint64_t>(std::round(rawDouble < 0.0 ? 0.0 : rawDouble));
    } else {
        rawValue = static_cast<uint64_t>(static_cast<int64_t>(std::round(rawDouble)));
    }
    injectRawSignalIntoMessage(msg, rawValue);
}


