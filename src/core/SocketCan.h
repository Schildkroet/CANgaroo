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

#pragma once

// SocketCAN representation of a BusMessage, per <linux/can.h> and
// <linux/can/error.h>: the canid_t with its flag bits and, for error frames,
// the error class bits and 8-byte error payload. Shared by the candump, pcap,
// pcapng and MDF4 writers so every format describes a frame the same way.
//
// Names are lower-case on purpose: <linux/can.h> defines CAN_EFF_FLAG & co as
// macros, which would break these declarations in any file including both.

#include <array>
#include <cstdint>

#include "core/BusMessage.h"

namespace SocketCan
{

// <linux/can.h>
inline constexpr std::uint32_t eff_flag = 0x80000000U;
inline constexpr std::uint32_t rtr_flag = 0x40000000U;
inline constexpr std::uint32_t err_flag = 0x20000000U;

// <linux/can/error.h>: error classes in can_id
inline constexpr std::uint32_t err_tx_timeout = 0x00000001U;
inline constexpr std::uint32_t err_crtl       = 0x00000004U;
inline constexpr std::uint32_t err_prot       = 0x00000008U;
inline constexpr std::uint32_t err_ack        = 0x00000020U;
inline constexpr std::uint32_t err_busoff     = 0x00000040U;
inline constexpr std::uint32_t err_buserror   = 0x00000080U;
inline constexpr std::uint32_t err_restarted  = 0x00000100U;

// <linux/can/error.h>: error payload
inline constexpr int err_dlc = 8;
inline constexpr std::uint8_t err_crtl_rx_overflow  = 0x01;  // data[1]
inline constexpr std::uint8_t err_crtl_rx_warning   = 0x04;  // data[1]
inline constexpr std::uint8_t err_crtl_tx_warning   = 0x08;  // data[1]
inline constexpr std::uint8_t err_crtl_rx_passive   = 0x10;  // data[1]
inline constexpr std::uint8_t err_crtl_tx_passive   = 0x20;  // data[1]
inline constexpr std::uint8_t err_crtl_active       = 0x40;  // data[1]
inline constexpr std::uint8_t err_prot_bit          = 0x01;  // data[2]
inline constexpr std::uint8_t err_prot_form         = 0x02;  // data[2]
inline constexpr std::uint8_t err_prot_stuff        = 0x04;  // data[2]
inline constexpr std::uint8_t err_prot_loc_crc_seq  = 0x08;  // data[3]

struct ErrorFrame
{
    std::uint32_t classes = 0;
    std::array<std::uint8_t, err_dlc> data{};
};

// Error classes and payload for an error frame's BusError flags.
[[nodiscard]] inline ErrorFrame errorFrame(const BusMessage &msg)
{
    const BusErrors flags = msg.errorFlags();
    ErrorFrame error;

    if (flags.testFlag(BusError::Bit))
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_bit;
    }
    if (flags.testFlag(BusError::Form))
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_form;
    }
    if (flags.testFlag(BusError::Stuff))
    {
        error.classes |= err_prot | err_buserror;
        error.data[2] |= err_prot_stuff;
    }
    if (flags.testFlag(BusError::Crc))
    {
        error.classes |= err_prot | err_buserror;
        error.data[3] = err_prot_loc_crc_seq;
    }
    if (flags.testFlag(BusError::Ack))
    {
        error.classes |= err_ack | err_buserror;
    }
    if (flags.testFlag(BusError::BusOff))
    {
        error.classes |= err_busoff;
    }
    if (flags.testFlag(BusError::Overrun))
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_overflow;
    }
    if (flags.testFlag(BusError::TxTimeout))
    {
        error.classes |= err_tx_timeout;
    }
    if (flags.testFlag(BusError::Restarted))
    {
        error.classes |= err_restarted;
    }
    // BusError does not record whether TX or RX errors caused the state
    // change, so both directions are flagged.
    if (flags.testFlag(BusError::ErrorWarning))
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_warning | err_crtl_tx_warning;
    }
    if (flags.testFlag(BusError::ErrorPassive))
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_rx_passive | err_crtl_tx_passive;
    }
    if (flags.testFlag(BusError::ErrorActive))
    {
        error.classes |= err_crtl;
        error.data[1] |= err_crtl_active;
    }

    // Generic or otherwise unclassified: an unspecified bus error, which is also
    // how python-can writes error frames. Without any class bit readers such as
    // python-can do not recognise the frame as an error frame at all.
    if (error.classes == 0)
    {
        error.classes = err_buserror;
    }
    return error;
}

// canid_t: identifier plus EFF / RTR flags, or ERR flag plus error classes.
[[nodiscard]] inline std::uint32_t canId(const BusMessage &msg)
{
    if (msg.isErrorFrame())
    {
        return err_flag | errorFrame(msg).classes;
    }
    std::uint32_t id = msg.getId();
    if (msg.isExtended()) { id |= eff_flag; }
    if (msg.isRTR())      { id |= rtr_flag; }
    return id;
}

}
