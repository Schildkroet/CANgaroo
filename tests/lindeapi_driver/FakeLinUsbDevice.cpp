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

#include "FakeLinUsbDevice.h"

#include <algorithm>
#include <cstring>

namespace
{

std::mutex g_presentMutex;
QList<FakeLinUsbDevice *> g_present;

} // namespace

FakeLinUsbDevice::FakeLinUsbDevice(Config config)
    : _boot(std::chrono::steady_clock::now()), _config(config)
{
    std::lock_guard lock(g_presentMutex);
    g_present.append(this);
}

FakeLinUsbDevice::~FakeLinUsbDevice()
{
    std::lock_guard lock(g_presentMutex);
    g_present.removeAll(this);
}

QList<FakeLinUsbDevice *> FakeLinUsbDevice::present()
{
    std::lock_guard lock(g_presentMutex);
    QList<FakeLinUsbDevice *> out;
    for (FakeLinUsbDevice *d : std::as_const(g_present))
    {
        std::lock_guard dl(d->_mutex);
        if (d->_plugged)
            out.append(d);
    }
    return out;
}

// ---- host side ----

bool FakeLinUsbDevice::claim()
{
    std::lock_guard lock(_mutex);
    // libusb_claim_interface() fails while another handle holds it
    if (!_plugged || _claimed)
        return false;
    _claimed = true;
    return true;
}

void FakeLinUsbDevice::release()
{
    std::lock_guard lock(_mutex);
    _claimed = false;
}

FakeLinUsbDevice::Result FakeLinUsbDevice::control(bool in, uint8_t bRequest, uint16_t wValue,
                                                   uint16_t wIndex, void *data, uint16_t len)
{
    std::lock_guard lock(_mutex);
    if (!_plugged)
        return Result::NoDevice;
    _requestLog.push_back(bRequest);
    const bool ok = controlSetup(in, bRequest, wValue, wIndex, data, len);
    if (!ok)
        ++_stalls;
    task();
    return ok ? Result::Ok : Result::Stall;
}

// linusb_control_xfer_cb(), SETUP and DATA stage in one go. The host side of
// the fake always sends vendor | interface requests.
bool FakeLinUsbDevice::controlSetup(bool in, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                    void *data, uint16_t len)
{
    if (wIndex != k_interface)
        return false;

    const uint8_t ch = static_cast<uint8_t>(wValue & 0xFFu);
    // The device's _ctrl_buf; tud_control_xfer() moves min(wLength, size) bytes
    union
    {
        lin_usb_bus_config_t baudrate;
        lin_usb_mode_t mode;
        lin_usb_schedule_entry_t schedule_entry;
        lin_usb_sleep_wakeup_t sleep_wakeup;
        lin_usb_device_config_t device_config;
        lin_usb_bus_state_t bus_state;
        uint8_t raw[64];
    } buf{};

    const auto receive = [&](std::size_t size) {
        if (!in && data != nullptr)
            std::memcpy(buf.raw, data, std::min<std::size_t>(len, size));
    };
    const auto send = [&](const void *src, std::size_t size) {
        if (in && data != nullptr)
            std::memcpy(data, src, std::min<std::size_t>(len, size));
        return true;
    };

    // ---- SETUP stage ----
    switch (bRequest)
    {
    case LIN_USB_BREQ_HOST_FORMAT:
        return true;  // data stage only, nothing is done with it
    case LIN_USB_BREQ_BAUDRATE:
        if (ch >= k_channels)
            return false;
        receive(sizeof(lin_usb_bus_config_t));
        break;
    case LIN_USB_BREQ_MODE:
        if (ch >= k_channels)
            return false;
        receive(sizeof(lin_usb_mode_t));
        break;
    case LIN_USB_BREQ_IDENTIFY:
        if (ch >= k_channels)
            return false;
        _channels[ch].identifies++;
        return true;
    case LIN_USB_BREQ_FRAME_CONFIG:
        if (ch >= k_channels)
            return false;
        receive(sizeof(lin_usb_schedule_entry_t));
        break;
    case LIN_USB_BREQ_SCHEDULE:
        if (ch >= k_channels)
            return false;
        if (((wValue >> 8) & 0xFFu) >= k_entries)
            return false;
        receive(sizeof(lin_usb_schedule_entry_t));
        break;
    case LIN_USB_BREQ_SLEEP_WAKEUP:
        if (ch >= k_channels)
            return false;
        receive(sizeof(lin_usb_sleep_wakeup_t));
        break;
    case LIN_USB_BREQ_DEVICE_CONFIG:
    {
        lin_usb_device_config_t &cfg = buf.device_config;
        cfg.schedule_tables = _config.tables;
        cfg.schedule_entries = k_entries;
        cfg.supported_baudrates = _config.baudrates;
        cfg.icount = k_channels - 1;
        cfg.sw_version = (0u << 16) | (1u << 8) | 0u;
        cfg.hw_version = _config.hwVersion;
        cfg.features = _config.features;
        return send(&cfg, sizeof(cfg));
    }
    case LIN_USB_BREQ_TIMESTAMP:
    {
        const uint32_t ts = tick();
        return send(&ts, sizeof(ts));
    }
    case LIN_USB_BREQ_BUS_STATE:
        if (ch >= k_channels)
            return false;
        buf.bus_state.state = _channels[ch].busState;
        buf.bus_state.dropped = _rxDropped[ch];
        return send(&buf.bus_state, sizeof(buf.bus_state));
    default:
        return false;
    }

    // ---- DATA stage (OUT requests) ----
    switch (bRequest)
    {
    case LIN_USB_BREQ_BAUDRATE:
        _channels[ch].config = buf.baudrate;
        break;
    case LIN_USB_BREQ_MODE:
        if (buf.mode.mode == LIN_USB_MODE_START && buf.mode.table_id >= _config.tables)
            return false;
        _channels[ch].mode = buf.mode;
        if (buf.mode.mode == LIN_USB_MODE_START)
            _channels[ch].busState = LIN_USB_BUS_STATE_OK;
        else if (buf.mode.mode == LIN_USB_MODE_STOP)
            _channels[ch].busState = LIN_USB_BUS_STATE_STOPPED;
        break;
    case LIN_USB_BREQ_FRAME_CONFIG:
        if (buf.schedule_entry.table_id >= _config.tables || buf.schedule_entry.dlc < 1u ||
            buf.schedule_entry.dlc > 8u)
            return false;
        _channels[ch].frameUpdates.push_back(buf.schedule_entry);
        break;
    case LIN_USB_BREQ_SCHEDULE:
    {
        const uint8_t slot = static_cast<uint8_t>((wValue >> 8) & 0xFFu);
        if (slot >= k_entries || buf.schedule_entry.table_id >= _config.tables ||
            buf.schedule_entry.dlc < 1u || buf.schedule_entry.dlc > 8u)
            return false;
        _channels[ch].schedule[buf.schedule_entry.table_id][slot] = buf.schedule_entry;
        break;
    }
    case LIN_USB_BREQ_SLEEP_WAKEUP:
        engineSleepWakeup(ch, buf.sleep_wakeup.command);
        break;
    default:
        break;
    }
    return true;
}

FakeLinUsbDevice::Result FakeLinUsbDevice::bulkIn(void *data, int len, int &transferred,
                                                  std::chrono::milliseconds timeout)
{
    transferred = 0;
    std::unique_lock lock(_mutex);
    // The frame in flight (_in_buf) waits at the endpoint for the host's IN token
    _cond.wait_for(lock, timeout, [&] { return !_plugged || (_inBusy && _hostPollsIn); });
    if (!_plugged)
        return Result::NoDevice;
    if (!_inBusy || !_hostPollsIn)
        return Result::Timeout;

    transferred = std::min<int>(len, sizeof(_inBuf));
    std::memcpy(data, &_inBuf, transferred);
    // linusb_xfer_cb(ep_in)
    _inBusy = false;
    inTrySend();
    task();
    _cond.notify_all();
    return Result::Ok;
}

FakeLinUsbDevice::Result FakeLinUsbDevice::bulkOut(const void *data, int len,
                                                   std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_mutex);
    // Un-armed OUT endpoint: the host is NAKed until lin_usb_task() re-arms it
    _cond.wait_for(lock, timeout, [&] { return !_plugged || _outArmed; });
    if (!_plugged)
        return Result::NoDevice;
    if (!_outArmed)
        return Result::Timeout;

    _outArmed = false;
    const int xferred = std::min<int>(len, sizeof(_outFrame));
    std::memset(&_outFrame, 0, sizeof(_outFrame));
    std::memcpy(&_outFrame, data, xferred);
    outXferDone(xferred);
    task();
    _cond.notify_all();
    return Result::Ok;
}

// linusb_xfer_cb(ep_out)
void FakeLinUsbDevice::outXferDone(int xferred)
{
    const bool valid = xferred == static_cast<int>(sizeof(lin_usb_host_frame_t)) &&
                       _outFrame.channel < k_channels && _outFrame.dlc >= 1u && _outFrame.dlc <= 8u;
    bool ok = false;
    if (valid)
    {
        ok = engineSetData(_outFrame);
    }
    if (xferred == static_cast<int>(sizeof(lin_usb_host_frame_t)))
        _ack = _outFrame;
    else
        std::memset(&_ack, 0, sizeof(_ack));
    _ack.echo_id = LIN_USB_ECHO_ID_SET_DATA_ACK;
    _ack.timestamp_ms = tick();
    _ack.flags = ok ? 0u : LIN_USB_FRAME_FLAG_ERROR;
    if (!reportFrameLocked(_ack, false))
    {
        _ackPending = true;
        return;
    }
    _outArmed = true;  // out_arm()
}

// ---- device side ----

bool FakeLinUsbDevice::reportFrame(const lin_usb_host_frame_t &frame)
{
    std::lock_guard lock(_mutex);
    const bool ok = reportFrameLocked(frame, true);
    _cond.notify_all();
    return ok;
}

bool FakeLinUsbDevice::reportFrameLocked(const lin_usb_host_frame_t &frame, bool countDrop)
{
    const bool ok = inQueuePush(frame, countDrop);
    inTrySend();
    return ok;
}

bool FakeLinUsbDevice::inQueuePush(const lin_usb_host_frame_t &frame, bool countDrop)
{
    const uint8_t next = static_cast<uint8_t>((_inHead + 1u) % k_inQueueSize);
    if (next == _inTail)
    {
        if (countDrop && frame.channel < k_channels && _rxDropped[frame.channel] < UINT16_MAX)
            _rxDropped[frame.channel]++;
        return false;
    }
    _inQueue[_inHead] = frame;
    _inHead = next;
    return true;
}

void FakeLinUsbDevice::inTrySend()
{
    if (_inBusy || _inTail == _inHead)
        return;
    _inBuf = _inQueue[_inTail];
    _inBusy = true;
    _inTail = static_cast<uint8_t>((_inTail + 1u) % k_inQueueSize);
}

// lin_usb_task(), USB part
void FakeLinUsbDevice::task()
{
    if (_ackPending)
    {
        if (inQueuePush(_ack, false))
        {
            _ackPending = false;
            _outArmed = true;
        }
    }
    inTrySend();
}

void FakeLinUsbDevice::unplug()
{
    std::lock_guard lock(_mutex);
    _plugged = false;
    _cond.notify_all();
}

void FakeLinUsbDevice::setTickOffset(uint32_t offset_ms)
{
    std::lock_guard lock(_mutex);
    _tickOffset = offset_ms;
}

uint32_t FakeLinUsbDevice::tick() const
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - _boot);
    return _tickOffset + static_cast<uint32_t>(ms.count());
}

// ---- LIN engine, as measured on the CANILFD adapter ----

uint8_t FakeLinUsbDevice::protectedId(uint8_t id)
{
    const auto bit = [id](int n) { return (id >> n) & 1; };
    const int p0 = bit(0) ^ bit(1) ^ bit(2) ^ bit(4);
    const int p1 = !(bit(1) ^ bit(3) ^ bit(4) ^ bit(5));
    return static_cast<uint8_t>((id & 0x3F) | (p0 << 6) | (p1 << 7));
}

bool FakeLinUsbDevice::running(uint8_t ch) const
{
    const auto &mode = _channels[ch].mode;
    return mode && mode->mode == LIN_USB_MODE_START;
}

// lin_engine_set_data()
bool FakeLinUsbDevice::engineSetData(const lin_usb_host_frame_t &frame)
{
    Channel &c = _channels[frame.channel];
    c.setData.push_back(frame);
    if (!running(frame.channel))
        return false;
    for (int slot = 0; slot < c.mode->entry_count && slot < k_entries; ++slot)
    {
        auto &e = c.schedule[c.mode->table_id][slot];
        if (e && e->lin_id == frame.lin_id)
        {
            std::memcpy(e->data, frame.data, std::min<int>(frame.dlc, 8));
            return true;
        }
    }
    return false;
}

// lin_engine_sleep_wakeup()
void FakeLinUsbDevice::engineSleepWakeup(uint8_t ch, uint8_t command)
{
    _channels[ch].sleepWakeup.push_back(command);
    if (!running(ch))
        return;
    lin_usb_host_frame_t f{};
    f.echo_id = LIN_USB_ECHO_ID_RX;
    f.timestamp_ms = tick();
    f.channel = ch;
    if (command == 0)
    {
        f.dlc = 8;
        f.flags = LIN_USB_FRAME_FLAG_SLEEP;
        std::memset(f.data + 1, 0xFF, 7);
        _channels[ch].busState = LIN_USB_BUS_STATE_SLEEPING;
    }
    else
    {
        f.flags = LIN_USB_FRAME_FLAG_WAKEUP;
        _channels[ch].busState = LIN_USB_BUS_STATE_OK;
    }
    reportFrameLocked(f, true);
}

void FakeLinUsbDevice::runSchedule(uint8_t ch)
{
    std::lock_guard lock(_mutex);
    if (!running(ch))
        return;
    const Channel &c = _channels[ch];
    for (int slot = 0; slot < c.mode->entry_count && slot < k_entries; ++slot)
    {
        const auto &e = c.schedule[c.mode->table_id][slot];
        if (!e)
            continue;
        lin_usb_host_frame_t f{};
        f.echo_id = LIN_USB_ECHO_ID_RX;
        f.timestamp_ms = tick();
        f.lin_id = protectedId(e->lin_id);
        f.channel = ch;
        f.dlc = e->dlc;
        if (e->direction == 0)
        {
            f.flags = LIN_USB_FRAME_FLAG_RESPONDED | LIN_USB_FRAME_FLAG_VALID;
            std::memcpy(f.data, e->data, e->dlc);
        }
        else
        {
            f.flags = LIN_USB_FRAME_FLAG_SUBSCRIBER | LIN_USB_FRAME_FLAG_ERROR;
        }
        reportFrameLocked(f, true);
    }
    _cond.notify_all();
}

void FakeLinUsbDevice::setBusState(uint8_t ch, uint8_t state)
{
    std::lock_guard lock(_mutex);
    _channels[ch].busState = state;
}

void FakeLinUsbDevice::setHostPollsIn(bool polls)
{
    std::lock_guard lock(_mutex);
    _hostPollsIn = polls;
    _cond.notify_all();
}

// ---- inspection ----

FakeLinUsbDevice::Channel FakeLinUsbDevice::channel(int ch) const
{
    std::lock_guard lock(_mutex);
    return _channels[ch];
}

std::vector<uint8_t> FakeLinUsbDevice::requestLog() const
{
    std::lock_guard lock(_mutex);
    return _requestLog;
}

int FakeLinUsbDevice::stalls() const
{
    std::lock_guard lock(_mutex);
    return _stalls;
}

bool FakeLinUsbDevice::claimed() const
{
    std::lock_guard lock(_mutex);
    return _claimed;
}

bool FakeLinUsbDevice::outArmed() const
{
    std::lock_guard lock(_mutex);
    return _outArmed;
}
