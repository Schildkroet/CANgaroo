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

#include <atomic>
#include <cstdint>

#include <QObject>
#include <QString>

#include "CanDriver.h"
#include "CanTiming.h"
#include "core/BusMessage.h"

class MeasurementInterface;

/// Abstract base class for a single physical or virtual bus channel.
///
/// Each BusInterface is owned by a CanDriver and represents one connectable
/// channel (e.g. one port of a USB CAN adapter). Subclasses implement the
/// driver-specific open/close/send/receive logic.
///
/// Thread safety: sendMessage() and readMessage() are called from a dedicated
/// BusListener thread; all other methods run on the main thread.
class BusInterface: public QObject
{
    Q_OBJECT

public:
    // -----------------------------------------------------------------------
    // Bus state codes — returned by getState()
    // -----------------------------------------------------------------------
    enum
    {
        state_ok,         ///< Bus is operational
        state_warning,    ///< Error counter exceeded warning threshold
        state_passive,    ///< Node is error-passive
        state_bus_off,    ///< Node has gone bus-off
        state_stopped,    ///< Interface is not running
        state_unknown,    ///< State cannot be determined
        state_tx_success, ///< Last TX completed successfully
        state_tx_fail,    ///< Last TX failed
    };

    // -----------------------------------------------------------------------
    // Capability flags — combined bitmask returned by getCapabilities()
    // -----------------------------------------------------------------------
    enum
    {
        // CAN capabilities
        capability_canfd                = 0x001, ///< CAN FD (ISO 11898-1:2015) frames
        capability_listen_only          = 0x002, ///< Passive / listen-only mode
        capability_triple_sampling      = 0x004, ///< Triple-sampling of the bus line
        capability_one_shot             = 0x008, ///< One-shot TX (no automatic retransmission)
        capability_auto_restart         = 0x010, ///< Auto-restart after bus-off
        capability_config_os            = 0x020, ///< Configurable OS / timestamp source
        capability_custom_bitrate       = 0x040, ///< Arbitrary CAN nominal bitrate
        capability_custom_canfd_bitrate = 0x080, ///< Arbitrary CAN FD data-phase bitrate
        capability_bus_off_recovery     = 0x800, ///< Host-triggered recovery, see recoverFromBusOff()
        capability_tdc                  = 0x1000, ///< Transmitter delay compensation for the CAN FD data phase
        // LIN capabilities
        capability_lin_master           = 0x100, ///< LIN master node (can send headers)
        capability_lin_slave            = 0x200, ///< LIN slave node
        capability_lin_monitor          = 0x400, ///< Passive LIN monitor
    };

public:
    // -----------------------------------------------------------------------
    // Construction / identity
    // -----------------------------------------------------------------------

    explicit BusInterface(CanDriver *driver);
    virtual ~BusInterface();

    /// Parent driver that created this interface.
    virtual CanDriver *getDriver();

    /// Short human-readable name (e.g. "can0", "PEAK PCAN-USB Ch1").
    virtual QString getName() const = 0;

    /// Optional longer description shown in the setup dialog.
    virtual QString getDetailsStr() const;

    /// Bus type (CAN or LIN); defaults to CAN.
    virtual BusType busType() const { return BusType::CAN; }

    /// Firmware / driver version string, if available.
    virtual QString getVersion();

    /// Unique numeric ID assigned by the backend at enumeration time.
    BusInterfaceId getId() const;
    void setId(BusInterfaceId id);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /// Apply measurement settings (bitrate, mode flags, …) before opening.
    virtual void applyConfig(const MeasurementInterface &mi) = 0;

    /// Currently active nominal bitrate in bits/s.
    virtual unsigned getBitrate() = 0;

    /// Bitmask of supported capability_* flags.
    virtual uint32_t getCapabilities();

    /// Bitrate presets the driver supports (shown in the setup dialog).
    virtual QList<CanTiming> getAvailableBitrates();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Open / start the hardware channel.  Call applyConfig() first.
    virtual void open();

    /// Stop and release the hardware channel.
    virtual void close();

    /// True while the channel is open and ready for traffic.
    virtual bool isOpen();

    // -----------------------------------------------------------------------
    // Message I/O
    // -----------------------------------------------------------------------

    /// Send one CAN or LIN message.  Called from the BusListener thread.
    virtual void sendMessage(const BusMessage &msg) = 0;

    /// Block for up to @p timeout_ms and fill @p msglist with received frames.
    /// Returns true when at least one message was appended.  Called from the
    /// BusListener thread in a tight loop.
    virtual bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) = 0;

    /// Send a LIN sleep or wakeup frame.  Only valid when capability_lin_master
    /// or capability_lin_slave is set.
    virtual void sendLinSleepWakeup(bool wakeup) { Q_UNUSED(wakeup) }

    /// Switch to a different LIN schedule table (master mode only).
    virtual void setLinScheduleTable(uint8_t tableIndex) { Q_UNUSED(tableIndex) }

    /// Send a LIN diagnostic (node-configuration) request frame.
    virtual void sendLinDiagRequest(uint8_t nad, const uint8_t *data, uint8_t len)
    { Q_UNUSED(nad); Q_UNUSED(data); Q_UNUSED(len); }

    /// Send a LIN diagnostic response frame (slave node answering a master request).
    /// Only valid when capability_lin_slave is set.
    virtual void sendLinDiagResponse(uint8_t nad, const uint8_t *data, uint8_t len)
    { Q_UNUSED(nad); Q_UNUSED(data); Q_UNUSED(len); }

    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------

    /// Refresh cached counters from hardware; returns false on failure.
    virtual bool updateStatistics();

    /// Reset all accumulated counters to zero.
    virtual void resetStatistics() { _totalBits = 0; }

    /// Current bus-state code (one of the state_* constants).
    virtual uint32_t getState() = 0;

    /// Ask the hardware to leave bus-off (only meaningful if the driver took over recovery).
    /// Returns false if unsupported or the request failed.
    virtual bool recoverFromBusOff() { return false; }

    virtual int getNumRxFrames() = 0;   ///< Total received frames since reset
    virtual int getNumRxErrors() = 0;   ///< Receive error count since reset
    virtual int getNumTxFrames() = 0;   ///< Total transmitted frames since reset
    virtual int getNumTxErrors() = 0;   ///< Transmit error count since reset
    virtual int getNumRxOverruns() = 0; ///< Receive buffer overruns since reset
    virtual int getNumTxDropped() = 0;  ///< Frames dropped before transmission since reset

    /// Accumulated bit count; used to calculate bus load percentage.
    virtual uint64_t getNumBits();

    /// Add the bit count of @p msg to the running total (called after TX/RX).
    void addFrameBits(const BusMessage &msg);

    /// Human-readable string for the current state (e.g. "Bus-off").
    QString getStateText();

private:
    BusInterfaceId          _id;
    CanDriver              *_driver;
    std::atomic<uint64_t>   _totalBits;
};
