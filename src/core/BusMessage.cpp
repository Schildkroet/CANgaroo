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

#include "BusMessage.h"

#include <algorithm>

#include <QStringList>

#include "core/portable_endian.h"

enum {
    id_flag_extended = 0x80000000,
    id_flag_rtr      = 0x40000000,
    id_flag_error    = 0x20000000,
    id_mask_extended = 0x1FFFFFFF,
    id_mask_standard = 0x7FF
};

BusMessage::BusMessage()
    : _raw_id(0), _dlc(0), _flags(0), _errorFlags{}, _isFD(false), _isBRS(false), _isRX(true), _isShow(true), _busType(BusType::CAN), _interface(0), _u8()
{
}

BusMessage::BusMessage(uint32_t can_id)
    : _raw_id(0), _dlc(0), _flags(0), _errorFlags{}, _isFD(false), _isBRS(false), _isRX(true), _isShow(true), _busType(BusType::CAN), _interface(0), _u8()
{
    setId(can_id);
}

BusType BusMessage::busType() const { return _busType; }
void    BusMessage::setBusType(BusType type) { _busType = type; }


uint32_t BusMessage::getRawId() const {
    return _raw_id & id_mask_extended;
}

void BusMessage::setRawId(const uint32_t raw_id) {
    _raw_id = raw_id;
}

uint32_t BusMessage::getId() const {
    if (isExtended()) {
        return _raw_id & id_mask_extended;
    } else {
        return _raw_id & id_mask_standard;
    }
}

void BusMessage::setId(const uint32_t id) {
    uint32_t flags = _raw_id & ~id_mask_extended;

    _raw_id = (id & id_mask_extended) | flags;
    if (id > 0x7FF)
    {
        setExtended(true);
    }
}

bool BusMessage::isExtended() const {
    return (_raw_id & id_flag_extended) != 0;
}

void BusMessage::setExtended(const bool isExtended) {
    if (isExtended) {
        _raw_id |= id_flag_extended;
    } else {
        _raw_id &= ~id_flag_extended;
    }
}

bool BusMessage::isRTR() const {
    return (_raw_id & id_flag_rtr) != 0;
}

void BusMessage::setRTR(const bool isRTR) {
    if (isRTR) {
        _raw_id |= id_flag_rtr;
    } else {
        _raw_id &= ~id_flag_rtr;
    }
}

bool BusMessage::isFD() const {
    return _isFD;
}

void BusMessage::setFD(const bool isFD) {
    _isFD = isFD;
}

bool BusMessage::isBRS() const {
    return _isBRS;
}

void BusMessage::setBRS(const bool isBRS) {
    _isBRS = isBRS;
}

bool BusMessage::isErrorFrame() const {
    return _errorFlags != 0;
}

void BusMessage::setErrorFrame(bool isErrorFrame) {
    if (isErrorFrame)
        _errorFlags |= BusError::Generic;
    else
        _errorFlags = {};
}

BusErrors BusMessage::errorFlags() const { return _errorFlags; }

void BusMessage::setErrorFlag(BusError flag) { _errorFlags |= flag; }
void BusMessage::clearErrorFlag(BusError flag) { _errorFlags &= ~BusErrors(flag); }
void BusMessage::setErrorFlags(BusErrors flags) { _errorFlags = flags; }

bool BusMessage::isLinSleepFrame()  const { return _busType == BusType::LIN && (_flags & lin_flag_sleep); }
bool BusMessage::isLinWakeupFrame() const { return _busType == BusType::LIN && (_flags & lin_flag_wakeup); }

BusInterfaceId BusMessage::getInterfaceId() const
{
    return _interface;
}

void BusMessage::setInterfaceId(BusInterfaceId id)
{
    _interface = id;
}

uint8_t BusMessage::getLength() const {
    return _dlc;
}

void BusMessage::setLength(const uint8_t dlc) {
    // Limit to CANFD max length
    if (dlc<=64) {
        _dlc = dlc;
    } else {
        _dlc = 8;
    }
}

uint8_t BusMessage::getFlags() const {
    return _flags;
}

void BusMessage::setFlags(uint8_t flags) {
    _flags = flags;
}

bool BusMessage::isRX() const {
    return _isRX;
}

void BusMessage::setRX(const bool isRX) {
    _isRX = isRX;
}

bool BusMessage::isShow() const {
    return _isShow;
}

void BusMessage::setShow(const bool enable) {
    _isShow = enable;
}

uint8_t BusMessage::getByte(const uint8_t index) const {
    if (index<sizeof(_u8)) {
        return _u8[index];
    } else {
        return 0;
    }
}

void BusMessage::setByte(const uint8_t index, const uint8_t value) {
    if (index<sizeof(_u8)) {
        _u8[index] = value;
    }
}

const uint8_t *BusMessage::getData() const
{
    return _u8;
}

uint64_t BusMessage::extractRawSignal(uint16_t start_bit, uint16_t length, bool isBigEndian) const
{
    if (length == 0 || length > 64 || start_bit >= sizeof(_u8) * 8) return 0;

    // Big-endian (Motorola) signals: DbcParser converts the DBC start bit into a
    // sequential MSB-first bit index (byte 0 first, MSB of each byte first), so the
    // signal is simply `length` consecutive bits of that bit stream starting at
    // start_bit. Walk it byte by byte, taking the bits from the most significant
    // end of each byte. The previous shift + conditional bswap64 treated the index
    // as an Intel-style LSB offset, which composes with neither convention: it
    // agreed with itself (so inject/extract round-tripped) but disagreed with every
    // other DBC tool for all but 60 of the 2080 possible start/length combinations.
    if (isBigEndian)
    {
        uint64_t data = 0;
        uint32_t byte_idx = start_bit / 8;
        uint32_t bit_off = start_bit % 8;

        for (uint16_t remaining = length; remaining > 0; byte_idx++, bit_off = 0)
        {
            const uint32_t avail = 8 - bit_off;
            const uint32_t take = std::min<uint32_t>(avail, remaining);
            const uint8_t byte = (byte_idx < sizeof(_u8)) ? _u8[byte_idx] : 0u;
            const uint8_t chunk = static_cast<uint8_t>((byte >> (avail - take)) & ((1u << take) - 1u));

            data = (data << take) | chunk;
            remaining = static_cast<uint16_t>(remaining - take);
        }

        return data;
    }

    int byte_offset = start_bit / 8;
    int bit_shift = start_bit % 8;

    uint8_t temp[8] = {0};
    int copy_len = sizeof(_u8) - byte_offset;
    if (copy_len > 8) copy_len = 8;

    memcpy(temp, _u8 + byte_offset, static_cast<size_t>(copy_len));

    uint64_t data_raw;
    memcpy(&data_raw, temp, sizeof(data_raw));
    data_raw = le64toh(data_raw);

    uint64_t data = data_raw >> bit_shift;

    uint64_t mask = 0xFFFFFFFFFFFFFFFF;
    if (length < 64) {
        mask = (1ULL << length) - 1;
    }
    data &= mask;

    return data;
}

void BusMessage::injectRawSignal(uint16_t start_bit, uint16_t length, bool isBigEndian, uint64_t value)
{
    if (length == 0 || length > 64 || start_bit >= sizeof(_u8) * 8) return;

    uint64_t mask = (length < 64) ? ((1ULL << length) - 1ULL) : ~0ULL;
    value &= mask;

    // Exact inverse of extractRawSignal's big-endian path: walk the bit stream
    // byte by byte, writing each chunk into the most significant end of the byte.
    // Bits past the frame's DLC are dropped rather than written.
    if (isBigEndian)
    {
        uint32_t byte_idx = start_bit / 8;
        uint32_t bit_off = start_bit % 8;

        for (uint16_t remaining = length; remaining > 0; byte_idx++, bit_off = 0)
        {
            const uint32_t avail = 8 - bit_off;
            const uint32_t take = std::min<uint32_t>(avail, remaining);
            remaining = static_cast<uint16_t>(remaining - take);

            if (byte_idx >= static_cast<uint32_t>(_dlc)) { continue; }

            const uint32_t shift = avail - take;
            const uint8_t chunk = static_cast<uint8_t>((value >> remaining) & ((1u << take) - 1u));
            const uint8_t byte_mask = static_cast<uint8_t>(((1u << take) - 1u) << shift);

            _u8[byte_idx] = static_cast<uint8_t>((_u8[byte_idx] & ~byte_mask) | (chunk << shift));
        }

        return;
    }

    int byte_offset = start_bit / 8;
    int bit_shift = start_bit % 8;

    uint8_t temp[8] = {0};
    int copy_len = static_cast<int>(sizeof(_u8)) - byte_offset;
    if (copy_len > 8) copy_len = 8;
    if (copy_len > 0)
        memcpy(temp, _u8 + byte_offset, static_cast<size_t>(copy_len));

    uint64_t data_raw;
    memcpy(&data_raw, temp, sizeof(data_raw));
    data_raw = le64toh(data_raw);

    data_raw &= ~(mask << bit_shift);
    data_raw |= value << bit_shift;

    data_raw = htole64(data_raw);
    memcpy(temp, &data_raw, sizeof(data_raw));

    for (int i = 0; i < copy_len; i++) {
        if (byte_offset + i < _dlc)
            _u8[byte_offset + i] = temp[i];
    }
}

void BusMessage::setDataAt(uint8_t position, uint8_t data)
{
    if(position < 64)
        _u8[position] = data;
    else
        return;
}

void BusMessage::setData(const uint8_t d0) {
    _dlc = 1;
    _u8[0] = d0;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1) {
    _dlc = 2;
    _u8[0] = d0;
    _u8[1] = d1;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2) {
    _dlc = 3;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2,
        const uint8_t d3) {
    _dlc = 4;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
    _u8[3] = d3;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2,
        const uint8_t d3, const uint8_t d4) {
    _dlc = 5;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
    _u8[3] = d3;
    _u8[4] = d4;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2,
        const uint8_t d3, const uint8_t d4, const uint8_t d5) {
    _dlc = 6;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
    _u8[3] = d3;
    _u8[4] = d4;
    _u8[5] = d5;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2,
        const uint8_t d3, const uint8_t d4, const uint8_t d5,
        const uint8_t d6) {
    _dlc = 7;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
    _u8[3] = d3;
    _u8[4] = d4;
    _u8[5] = d5;
    _u8[6] = d6;
}

void BusMessage::setData(const uint8_t d0, const uint8_t d1, const uint8_t d2,
        const uint8_t d3, const uint8_t d4, const uint8_t d5, const uint8_t d6,
        const uint8_t d7) {
    _dlc = 8;
    _u8[0] = d0;
    _u8[1] = d1;
    _u8[2] = d2;
    _u8[3] = d3;
    _u8[4] = d4;
    _u8[5] = d5;
    _u8[6] = d6;
    _u8[7] = d7;
}

int64_t BusMessage::getTimestamp_us() const
{
    return _timestamp_us;
}

int64_t BusMessage::getTimestamp_ms() const
{
    return _timestamp_us / 1000;
}

double BusMessage::getFloatTimestamp() const
{
    return static_cast<double>(_timestamp_us) / 1000000.0;
}

QDateTime BusMessage::getDateTime() const
{
    return QDateTime::fromMSecsSinceEpoch(_timestamp_us / 1000, Qt::LocalTime);
}

void BusMessage::setTimestamp_us(int64_t us)
{
    _timestamp_us = us;
}

void BusMessage::setTimestamp_ms(int64_t ms)
{
    _timestamp_us = ms * 1000;
}

void BusMessage::setTimestamp(double seconds)
{
    _timestamp_us = static_cast<int64_t>(seconds * 1000000.0);
}

void BusMessage::setTimestamp(uint64_t seconds, uint32_t micro_seconds)
{
    _timestamp_us = static_cast<int64_t>(seconds) * 1000000 + micro_seconds;
}

QString BusMessage::getIdString() const
{
    if (_busType == BusType::LIN)
        return QStringLiteral("0x%1").arg(getId() & 0x3F, 2, 16, QLatin1Char('0')).toUpper();

    if (isExtended()) {
        return QStringLiteral("0x%1").arg(getId(), 8, 16, QLatin1Char('0')).toUpper();
    } else {
        return QStringLiteral("0x%1").arg(getId(), 3, 16, QLatin1Char('0')).toUpper();
    }
}

QString BusMessage::getDataHexString() const
{
    if(getLength() == 0)
        return QString();

    if(isErrorFrame())
        return getErrorFlagsString();

    static const char hex[] = "0123456789ABCDEF";
    int len = getLength();
    QString outstr(len * 3, Qt::Uninitialized);
    QChar *p = outstr.data();
    for(int i = 0; i < len; i++)
    {
        uint8_t b = getByte(i);
        *p++ = QLatin1Char(hex[b >> 4]);
        *p++ = QLatin1Char(hex[b & 0x0F]);
        *p++ = QLatin1Char(' ');
    }

    return outstr;
}

QString BusMessage::getDataAsciiString() const
{
    if(getLength() == 0)
        return QString();

    if(isErrorFrame())
        return getErrorFlagsString();

    int len = getLength();
    QString outstr(len * 2, Qt::Uninitialized);
    QChar *p = outstr.data();
    for(int i = 0; i < len; i++)
    {
        uint8_t b = getByte(i);
        *p++ = (b >= 0x20 && b < 0x7F) ? QLatin1Char(static_cast<char>(b)) : QLatin1Char('.');
        *p++ = QLatin1Char(' ');
    }

    return outstr;
}

QString BusMessage::formatDataBytes(const QByteArray &data, bool ascii)
{
    const int sz = data.size();
    if (sz == 0) return QString();

    if (ascii)
    {
        QString result(sz * 2, Qt::Uninitialized);
        QChar *p = result.data();
        for (int i = 0; i < sz; ++i)
        {
            uint8_t b = static_cast<uint8_t>(data.at(i));
            *p++ = (b >= 0x20 && b < 0x7F) ? QLatin1Char(static_cast<char>(b)) : QLatin1Char('.');
            *p++ = QLatin1Char(' ');
        }
        return result;
    }

    static const char hex[] = "0123456789ABCDEF";
    QString result(sz * 3 - 1, ' ');
    QChar *p = result.data();
    for (int i = 0; i < sz; ++i)
    {
        uint8_t b = static_cast<uint8_t>(data.at(i));
        if (i > 0) ++p; // skip the pre-filled space
        *p++ = QLatin1Char(hex[b >> 4]);
        *p++ = QLatin1Char(hex[b & 0x0F]);
    }
    return result;
}

QString BusMessage::getErrorFlagsString() const
{
    static const struct { BusError flag; const char *name; } kNames[] = {
        { BusError::Ack,      "ACK"     },
        { BusError::Bit,      "BIT"     },
        { BusError::Stuff,    "STUFF"   },
        { BusError::Crc,      "CRC"     },
        { BusError::Form,     "FORM"    },
        { BusError::BusOff,   "BUSOFF"  },
        { BusError::Overrun,  "OVERRUN" },
        { BusError::TxTimeout,        "TIMEOUT"        },
        { BusError::LinNotResponded,  "NOT RESPONDED"  },
        { BusError::LinChecksumError, "CHECKSUM ERROR" },
        { BusError::Restarted,        "RESTARTED"      },
        { BusError::ErrorWarning,     "WARNING"        },
        { BusError::ErrorPassive,     "PASSIVE"        },
        { BusError::ErrorActive,      "ACTIVE"         },
        { BusError::Generic,          "ERROR"          },
    };
    QStringList parts;
    for (const auto &e : kNames)
        if (_errorFlags.testFlag(e.flag))
            parts.append(QLatin1String(e.name));
    if (parts.isEmpty())
        return QStringLiteral("ERROR");
    return QStringLiteral("ERROR: ") + parts.join(QLatin1Char('+'));
}
