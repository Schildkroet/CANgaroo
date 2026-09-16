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

#include <cstdint>

#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QFlags>

#include "driver/CanDriver.h"

enum class BusType : uint8_t
{
    CAN = 0,
    LIN = 1,
};

enum class BusError : uint32_t
{
    Ack       = 0x0001,
    Bit       = 0x0002,
    Stuff     = 0x0004,
    Crc       = 0x0008,
    Form      = 0x0010,
    BusOff    = 0x0020,
    Overrun   = 0x0040,
    TxTimeout        = 0x0080,
    LinNotResponded  = 0x0100,
    LinChecksumError = 0x0200,
    Restarted        = 0x0400,   // controller recovered from bus-off
    ErrorWarning     = 0x0800,   // controller reached the error-warning level
    ErrorPassive     = 0x1000,   // controller reached the error-passive level
    ErrorActive      = 0x2000,   // controller back to error-active
    Generic          = 0x8000,
};
Q_DECLARE_FLAGS(BusErrors, BusError)
Q_DECLARE_OPERATORS_FOR_FLAGS(BusErrors)

class BusMessage
{
public:
    static constexpr int k_maxDataBytes = 64; // CAN-FD maximum payload

    BusMessage();
    BusMessage(uint32_t can_id);
    BusMessage(const BusMessage &) = default;
    BusMessage& operator=(const BusMessage&) = default;

    [[nodiscard]] BusType busType() const;
    void setBusType(BusType type);

    [[nodiscard]] uint32_t getRawId() const;
    void setRawId(const uint32_t raw_id);

    [[nodiscard]] uint32_t getId() const;
    void setId(const uint32_t id);

    [[nodiscard]] bool isExtended() const;
    void setExtended(const bool isExtended);

    [[nodiscard]] bool isRTR() const;
    void setRTR(const bool isRTR);

    [[nodiscard]] bool isFD() const;
    void setFD(const bool isFD);

    [[nodiscard]] bool isBRS() const;
    void setBRS(const bool isBRS);

    [[nodiscard]] bool isErrorFrame() const;
    void setErrorFrame(bool isErrorFrame);

    // LIN event bits carried in getFlags() / setFlags()
    static constexpr uint8_t lin_flag_sleep  = 0x04u;
    static constexpr uint8_t lin_flag_wakeup = 0x08u;

    [[nodiscard]] bool isLinSleepFrame()  const;
    [[nodiscard]] bool isLinWakeupFrame() const;

    [[nodiscard]] BusErrors errorFlags() const;
    void setErrorFlag(BusError flag);
    void clearErrorFlag(BusError flag);
    void setErrorFlags(BusErrors flags);

    [[nodiscard]] BusInterfaceId getInterfaceId() const;
    void setInterfaceId(BusInterfaceId id);

    [[nodiscard]] uint8_t getLength() const;
    void setLength(const uint8_t dlc);

    [[nodiscard]] bool isRX() const;
    void setRX(const bool isRX);

    [[nodiscard]] bool isShow() const;
    void setShow(const bool enable);

    [[nodiscard]] uint8_t getFlags() const;
    void setFlags(uint8_t flags);

    [[nodiscard]] uint8_t getByte(const uint8_t index) const;
    void setByte(const uint8_t index, const uint8_t value);

    [[nodiscard]] const uint8_t *getData() const;

    [[nodiscard]] uint64_t extractRawSignal(uint16_t start_bit, uint16_t length, bool isBigEndian) const;
    void injectRawSignal(uint16_t start_bit, uint16_t length, bool isBigEndian, uint64_t value);

    void setDataAt(uint8_t position, uint8_t data);
    void setData(const uint8_t d0);
    void setData(const uint8_t d0, const uint8_t d1);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2, const uint8_t d3);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2, const uint8_t d3, const uint8_t d4);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2, const uint8_t d3, const uint8_t d4, const uint8_t d5);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2, const uint8_t d3, const uint8_t d4, const uint8_t d5, const uint8_t d6);
    void setData(const uint8_t d0, const uint8_t d1, const uint8_t d2, const uint8_t d3, const uint8_t d4, const uint8_t d5, const uint8_t d6, const uint8_t d7);

    [[nodiscard]] int64_t getTimestamp_us() const;
    [[nodiscard]] int64_t getTimestamp_ms() const;
    [[nodiscard]] double getFloatTimestamp() const;
    [[nodiscard]] QDateTime getDateTime() const;

    void setTimestamp_us(int64_t us);
    void setTimestamp_ms(int64_t ms);
    void setTimestamp(double seconds);
    void setTimestamp(uint64_t seconds, uint32_t micro_seconds);

    [[nodiscard]] QString getIdString() const;
    [[nodiscard]] QString getDataHexString() const;
    [[nodiscard]] QString getDataAsciiString() const;
    [[nodiscard]] QString getErrorFlagsString() const;

    // General-purpose byte formatter for payloads that aren't a full BusMessage
    // (e.g. a decoded ProtocolMessage's payload), sharing the same hex/ASCII
    // rendering used by getDataHexString()/getDataAsciiString().
    [[nodiscard]] static QString formatDataBytes(const QByteArray &data, bool ascii);

private:
    uint32_t _raw_id;
    uint8_t _dlc;
    uint8_t _flags;
    BusErrors _errorFlags;
    bool _isFD;
    bool _isBRS;
    bool _isRX;
    bool _isShow;
    BusType _busType;
    BusInterfaceId _interface;
    union {
        uint8_t  _u8[k_maxDataBytes];
        uint16_t _u16[k_maxDataBytes / 2];
        uint32_t _u32[k_maxDataBytes / 4];
        uint64_t _u64[k_maxDataBytes / 8];
    };

    int64_t _timestamp_us = 0;

};
