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

#include "FakeGsUsbDevice.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace
{

// enum gs_usb_breq (gs_usb.h), Elmue's eUsbRequest has the same numbers
enum : uint8_t {
    BREQ_HOST_FORMAT = 0, BREQ_BITTIMING, BREQ_MODE, BREQ_BERR, BREQ_BT_CONST,
    BREQ_DEVICE_CONFIG, BREQ_TIMESTAMP, BREQ_IDENTIFY, BREQ_GET_USER_ID, BREQ_SET_USER_ID,
    BREQ_DATA_BITTIMING, BREQ_BT_CONST_EXT, BREQ_SET_TERMINATION, BREQ_GET_TERMINATION,
    BREQ_GET_STATE, BREQ_SET_FILTER, BREQ_GET_FILTER, BREQ_GET_TDC_CONST, BREQ_SET_TDC,
    BREQ_GET_TDC,
    ELM_REQ_FIRST = 20, ELM_REQ_GET_LAST_ERROR = 22,
    BREQ_BUS_OFF_RECOVERY = 32,
};

// GS_CAN_FEATURE_* / GS_DevFlag* (bit numbers are shared)
constexpr uint32_t F_LISTEN_ONLY = 1u << 0;
constexpr uint32_t F_LOOP_BACK = 1u << 1;
constexpr uint32_t F_ONE_SHOT = 1u << 3;
constexpr uint32_t F_HW_TIMESTAMP = 1u << 4;
constexpr uint32_t F_IDENTIFY = 1u << 5;
constexpr uint32_t F_PAD_PKTS = 1u << 7;
constexpr uint32_t F_FD = 1u << 8;
constexpr uint32_t F_BT_CONST_EXT = 1u << 10;
constexpr uint32_t F_BERR_REPORTING = 1u << 12;
constexpr uint32_t F_GET_STATE = 1u << 13;
constexpr uint32_t F_ELM_PROTOCOL = 1u << 14;
constexpr uint32_t F_ELM_BLOBS = 1u << 15;
constexpr uint32_t F_BUS_OFF_RECOVERY = 1u << 18;

constexpr uint8_t FLAG_OVERFLOW = 0x01;
constexpr uint8_t FLAG_FD = 0x02;
constexpr uint8_t FLAG_BRS = 0x04;
constexpr uint8_t FLAG_ESI = 0x08;

constexpr uint32_t CAN_EFF_FLAG = 0x80000000u;
constexpr uint32_t CAN_RTR_FLAG = 0x40000000u;
constexpr uint32_t CAN_ERR_FLAG = 0x20000000u;
constexpr uint32_t CAN_ERR_CRTL = 0x00000004u;
constexpr uint32_t CAN_ERR_BUSOFF = 0x00000040u;
constexpr uint32_t CAN_ERR_RESTARTED = 0x00000100u;
constexpr uint32_t CAN_ERR_CNT = 0x00000200u;
constexpr uint8_t CAN_ERR_CRTL_RX_WARNING = 0x04;
constexpr uint8_t CAN_ERR_CRTL_TX_WARNING = 0x08;
constexpr uint8_t CAN_ERR_CRTL_RX_PASSIVE = 0x10;
constexpr uint8_t CAN_ERR_CRTL_TX_PASSIVE = 0x20;
constexpr uint8_t CAN_ERR_CRTL_ACTIVE = 0x40;

constexpr uint32_t ECHO_ID_RX = 0xFFFFFFFFu;
constexpr int HEADER = 12;  // echo_id, can_id, dlc, channel, flags, reserved

constexpr std::array<uint8_t, 16> k_dlcToBytes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

void put32(QByteArray &out, int pos, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out[pos + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void put32raw(uint8_t *out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

FakeGsUsbDevice::State stateFor(uint32_t err)
{
    // can_err_to_state(): 96 / 128 / 256
    if (err < 96)
        return FakeGsUsbDevice::State::ErrorActive;
    if (err < 128)
        return FakeGsUsbDevice::State::ErrorWarning;
    if (err < 256)
        return FakeGsUsbDevice::State::ErrorPassive;
    return FakeGsUsbDevice::State::BusOff;
}

} // namespace


// ── Registry ─────────────────────────────────────────────────────────────────

std::vector<FakeGsUsbDevice *> &FakeGsUsbDevice::present()
{
    static std::vector<FakeGsUsbDevice *> devices;
    return devices;
}

FakeGsUsbDevice *FakeGsUsbDevice::byPath(const wchar_t *path)
{
    for (FakeGsUsbDevice *d : present())
        if (d->_path == path)
            return d;
    return nullptr;
}

FakeGsUsbDevice::FakeGsUsbDevice(Firmware fw, int channels, std::wstring path)
    : _fw(fw), _path(std::move(path)), _ch(static_cast<size_t>(channels))
{
    present().push_back(this);
}

FakeGsUsbDevice::~FakeGsUsbDevice()
{
    auto &p = present();
    p.erase(std::remove(p.begin(), p.end(), this), p.end());
}


// ── Static properties ────────────────────────────────────────────────────────

uint32_t FakeGsUsbDevice::clockHz() const
{
    return _fw == Firmware::CandleLightF072 ? 48000000u : 160000000u;
}

uint32_t FakeGsUsbDevice::feature() const
{
    if (_fw == Firmware::CandleLightF072)
    {
        // bxcan.c CAN_btconst (no termination / filter on this board)
        return F_LISTEN_ONLY | F_LOOP_BACK | F_ONE_SHOT | F_HW_TIMESTAMP | F_IDENTIFY | F_PAD_PKTS |
               F_BERR_REPORTING | F_GET_STATE | F_BUS_OFF_RECOVERY;
    }
    // control_init(): GS_CapabilityClassic.feature (no termination pins on Multiboard)
    return F_LISTEN_ONLY | F_LOOP_BACK | F_ONE_SHOT | F_HW_TIMESTAMP | F_IDENTIFY | F_FD |
           F_BT_CONST_EXT | F_GET_STATE | F_ELM_PROTOCOL | F_ELM_BLOBS;
}

uint32_t FakeGsUsbDevice::timestampUs() const
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - _t0).count());
}


// ── Test controls / inspection ───────────────────────────────────────────────

void FakeGsUsbDevice::setInUse(bool inUse)
{
    std::lock_guard lock(_mutex);
    _inUse = inUse;
}

void FakeGsUsbDevice::unplug()
{
    std::lock_guard lock(_mutex);
    _unplugged = true;
    _cond.notify_all();
}

void FakeGsUsbDevice::leaveRunning(int ch, const Timing &nominal)
{
    std::lock_guard lock(_mutex);
    _ch[ch].nominal = nominal;
    startChannel(ch, 0);
}

bool FakeGsUsbDevice::started(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].started; }
uint32_t FakeGsUsbDevice::startFlags(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].flags; }
std::optional<FakeGsUsbDevice::Timing> FakeGsUsbDevice::nominalTiming(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].nominal; }
std::optional<FakeGsUsbDevice::Timing> FakeGsUsbDevice::dataTiming(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].data; }
bool FakeGsUsbDevice::listenOnly(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].started && (_ch[ch].flags & F_LISTEN_ONLY); }
QList<FakeGsUsbDevice::Frame> FakeGsUsbDevice::transmitted(int ch) const { std::lock_guard lock(_mutex); return _ch[ch].transmitted; }
QList<uint8_t> FakeGsUsbDevice::stalled() const { std::lock_guard lock(_mutex); return _stalled; }
QList<uint8_t> FakeGsUsbDevice::ignored() const { std::lock_guard lock(_mutex); return _ignored; }
int FakeGsUsbDevice::bulkInCalls() const { std::lock_guard lock(_mutex); return _bulkInCalls; }
int FakeGsUsbDevice::usbOpenCount() const { std::lock_guard lock(_mutex); return _usbOpen; }


// ── Channel state ────────────────────────────────────────────────────────────

void FakeGsUsbDevice::startChannel(int ch, uint32_t flags)
{
    Channel &c = _ch[ch];
    c.started = true;
    c.flags = flags;
    c.txErr = c.rxErr = 0;
    c.state = State::ErrorActive;
    // Upstream keeps frames that came while stopped (can_send() fails, the
    // frame is put back); they go out now.
    while (!c.pendingTx.empty())
    {
        const QByteArray f = c.pendingTx.front();
        c.pendingTx.pop_front();
        transmit(ch, f);
    }
}

void FakeGsUsbDevice::stopChannel(int ch)
{
    Channel &c = _ch[ch];
    const bool wasOpen = c.started;
    c.started = false;
    c.state = State::Stopped;
    c.pendingTx.clear();  // usbd_gs_can_purge_from_host_list_by_channel()
    if (_fw == Firmware::ElmueG431 && wasOpen)
    {
        // can_close() -> can_reset(): "Brp = 0, invalid = baudrate not set"
        c.nominal.reset();
        c.data.reset();
    }
}

void FakeGsUsbDevice::setErrorCounters(int ch, uint32_t txErr, uint32_t rxErr)
{
    std::lock_guard lock(_mutex);
    _ch[ch].txErr = txErr;
    _ch[ch].rxErr = rxErr;
    updateState(ch);
    _cond.notify_all();
}

// can_state_change_pending() / can_handle_state_change() (upstream only;
// Elmue's legacy protocol has no error frames)
void FakeGsUsbDevice::updateState(int ch)
{
    Channel &c = _ch[ch];
    if (!c.started)
        return;
    const State txState = stateFor(c.txErr);
    const State rxState = stateFor(c.rxErr);
    const State newState = std::max(txState, rxState);
    if (newState == c.state)
        return;
    c.state = newState;
    if (_fw != Firmware::CandleLightF072)
        return;

    if (newState == State::BusOff)
    {
        errorFrame(ch, CAN_ERR_BUSOFF, 0);
        if (!(c.flags & F_BUS_OFF_RECOVERY))
        {
            // "host is not taking care of CAN bus off recovery": the firmware
            // restarts after CAN_BUS_OFF_RESTART_DELAY_MS (100 ms; not timed here)
            c.txErr = c.rxErr = 0;
            c.state = State::ErrorActive;
            errorFrame(ch, CAN_ERR_RESTARTED, 0);
        }
        return;
    }

    const auto txBits = [](State s) -> uint8_t {
        return s == State::ErrorActive ? CAN_ERR_CRTL_ACTIVE
             : s == State::ErrorWarning ? CAN_ERR_CRTL_TX_WARNING
             : s == State::ErrorPassive ? CAN_ERR_CRTL_TX_PASSIVE : 0;
    };
    const auto rxBits = [](State s) -> uint8_t {
        return s == State::ErrorActive ? CAN_ERR_CRTL_ACTIVE
             : s == State::ErrorWarning ? CAN_ERR_CRTL_RX_WARNING
             : s == State::ErrorPassive ? CAN_ERR_CRTL_RX_PASSIVE : 0;
    };
    uint8_t data1 = 0;
    if (txState >= rxState)
        data1 |= txBits(txState);
    if (txState <= rxState)
        data1 |= rxBits(rxState);
    errorFrame(ch, CAN_ERR_CRTL | CAN_ERR_CNT, data1);
}

void FakeGsUsbDevice::errorFrame(int ch, uint32_t classes, uint8_t data1)
{
    // can_prepare_error_frame()
    QByteArray f(HEADER + 8, '\0');
    put32(f, 0, ECHO_ID_RX);
    put32(f, 4, CAN_ERR_FLAG | classes);
    f[8] = 8;  // CAN_ERR_DLC
    f[9] = static_cast<char>(ch);
    f[HEADER + 1] = static_cast<char>(data1);
    if (classes & CAN_ERR_CNT)
    {
        f[HEADER + 6] = static_cast<char>(std::min<uint32_t>(_ch[ch].txErr, 255));
        f[HEADER + 7] = static_cast<char>(std::min<uint32_t>(_ch[ch].rxErr, 255));
    }
    toHost(ch, f);
}


// ── Frames ───────────────────────────────────────────────────────────────────

// USBD_GS_CAN_SendFrame() / buf_process_host() (legacy): classic 20, FD 76,
// +4 with a hardware timestamp; upstream pads classic frames to 64 on request.
void FakeGsUsbDevice::toHost(int ch, QByteArray frame)
{
    const Channel &c = _ch[ch];
    const bool fd = static_cast<uint8_t>(frame[10]) & FLAG_FD;
    frame.resize(fd ? HEADER + 64 : HEADER + 8, '\0');
    if (c.flags & F_HW_TIMESTAMP)
    {
        frame.resize(frame.size() + 4);
        put32(frame, frame.size() - 4, timestampUs());
    }
    if (_overflowNext)
    {
        frame[10] = static_cast<char>(static_cast<uint8_t>(frame[10]) | FLAG_OVERFLOW);
        _overflowNext = false;
    }
    if (_fw == Firmware::CandleLightF072 && (c.flags & F_PAD_PKTS) && !fd)
        frame.resize(64, '\0');
    _toHost.push_back(frame);
    _cond.notify_all();
}

void FakeGsUsbDevice::receiveBusFrame(int ch, const Frame &frame)
{
    std::lock_guard lock(_mutex);
    if (!_ch[ch].started)
        return;
    if (frame.fd && _fw == Firmware::CandleLightF072)
        return;  // bxCAN cannot receive CAN FD frames

    QByteArray f(HEADER, '\0');
    put32(f, 0, ECHO_ID_RX);
    put32(f, 4, frame.id | (frame.extended ? CAN_EFF_FLAG : 0) | (frame.rtr ? CAN_RTR_FLAG : 0));
    f[8] = static_cast<char>(frame.dlc);
    f[9] = static_cast<char>(ch);
    f[10] = static_cast<char>((frame.fd ? FLAG_FD : 0) | (frame.brs ? FLAG_BRS : 0));
    if (!frame.rtr)
        f += frame.data;
    toHost(ch, f);
}

// A host frame accepted for transmission: on the bus, and echoed at once.
void FakeGsUsbDevice::transmit(int ch, const QByteArray &hostFrame)
{
    Channel &c = _ch[ch];
    const auto *p = reinterpret_cast<const uint8_t *>(hostFrame.constData());
    const uint32_t canId = get32(p + 4);
    uint8_t dlc = p[8];
    uint8_t flags = p[10];
    if (_fw == Firmware::ElmueG431 && dlc > 8)
        flags |= FLAG_FD;  // "if (can_dlc > 8) flags |= FRM_FDF"
    const bool fd = _fw == Firmware::ElmueG431 && (flags & FLAG_FD);
    if (!fd)
        dlc = std::min<uint8_t>(dlc, 8);

    // Assumption (bxCAN silent mode): the frame completes internally, so the
    // upstream firmware echoes it, but it never reaches the bus.
    if (!(c.flags & F_LISTEN_ONLY))
    {
        Frame onBus;
        onBus.extended = canId & CAN_EFF_FLAG;
        onBus.rtr = canId & CAN_RTR_FLAG;
        onBus.id = canId & (onBus.extended ? 0x1FFFFFFFu : 0x7FFu);
        onBus.fd = fd;
        onBus.brs = fd && (flags & FLAG_BRS);
        onBus.dlc = dlc;
        if (!onBus.rtr)
            onBus.data = hostFrame.mid(HEADER, k_dlcToBytes.at(fd ? dlc : std::min<uint8_t>(dlc, 8)));
        c.transmitted.append(onBus);
    }

    // Echo: the host frame with its echo_id, reserved = 0 and a timestamp
    QByteArray echo = hostFrame.left(fd ? HEADER + 64 : HEADER + 8);
    echo.resize(fd ? HEADER + 64 : HEADER + 8, '\0');
    echo[8] = static_cast<char>(dlc);
    echo[10] = static_cast<char>(fd ? (flags & (FLAG_FD | FLAG_BRS)) : 0);
    echo[11] = 0;
    if (_fw == Firmware::ElmueG431 && fd && (c.state == State::ErrorPassive))
        echo[10] = static_cast<char>(static_cast<uint8_t>(echo[10]) | FLAG_ESI);
    toHost(ch, echo);
}

bool FakeGsUsbDevice::bulkOut(const uint8_t *data, uint32_t len)
{
    std::lock_guard lock(_mutex);
    if (_unplugged)
        return false;
    if (len < HEADER + 8)
        return true;  // upstream: "Invalid frame length, just ignore it"
    const int ch = data[9];
    if (ch >= static_cast<int>(_ch.size()))
        return true;  // invalid channel: dropped
    const QByteArray frame(reinterpret_cast<const char *>(data), static_cast<int>(len));
    Channel &c = _ch[ch];

    if (_fw == Firmware::CandleLightF072)
    {
        if (c.started)
            transmit(ch, frame);
        else
            c.pendingTx.push_back(frame);
        return true;
    }

    // Elmue, buf_store_can_frame() + buf_process_can(): dropped (no echo) for
    // an FD frame without data bitrate, while closed, silent or bus off
    const bool fd = (data[10] & FLAG_FD) || data[8] > 8;
    if (fd && !c.data)
        return true;
    if (!c.started || (c.flags & F_LISTEN_ONLY) || c.state == State::BusOff)
        return true;
    transmit(ch, frame);
    return true;
}

int FakeGsUsbDevice::bulkIn(uint8_t *buf, uint32_t bufsize, uint32_t *len, uint32_t timeoutMs)
{
    std::unique_lock lock(_mutex);
    ++_bulkInCalls;
    if (_unplugged)
        return 2;
    if (!_cond.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return !_toHost.empty() || _unplugged; }))
        return 1;
    if (_unplugged)
        return 2;
    const QByteArray f = _toHost.front();
    _toHost.pop_front();
    const auto n = static_cast<uint32_t>(std::min<qsizetype>(f.size(), bufsize));
    std::memcpy(buf, f.constData(), n);
    *len = n;
    return 0;
}


// ── Open / control ───────────────────────────────────────────────────────────

bool FakeGsUsbDevice::usbOpen()
{
    std::lock_guard lock(_mutex);
    if (_inUse || _unplugged)
        return false;
    ++_usbOpen;
    return true;
}

void FakeGsUsbDevice::usbClose()
{
    std::lock_guard lock(_mutex);
    if (_usbOpen > 0)
        --_usbOpen;
    _cond.notify_all();
}

bool FakeGsUsbDevice::timingOk(bool dataPhase, const Timing &t) const
{
    if (_fw == Firmware::CandleLightF072)
    {
        // can_check_bittiming_ok() against CAN_btconst.btc (bxcan.c)
        const uint32_t tseg1 = t.prop + t.seg1;
        return !dataPhase && tseg1 >= 1 && tseg1 <= 16 && t.seg2 >= 1 && t.seg2 <= 8 &&
               t.sjw <= 4 && t.brp >= 1 && t.brp <= 1024;
    }
    // can_set_bit_timing(brp, prop + seg1, seg2, sjw): IS_FDCAN_* ranges, and
    // not above MAX_CAN_BAUDRATE (10 MBaud)
    const uint32_t seg1 = t.prop + t.seg1;
    const bool ok = dataPhase
        ? (t.brp >= 1 && t.brp <= 32 && seg1 >= 1 && seg1 <= 32 && t.seg2 >= 1 && t.seg2 <= 16 && t.sjw >= 1 && t.sjw <= 16)
        : (t.brp >= 1 && t.brp <= 512 && seg1 >= 1 && seg1 <= 256 && t.seg2 >= 1 && t.seg2 <= 128 && t.sjw >= 1 && t.sjw <= 128);
    return ok && double(clockHz()) / (double(t.brp) * t.quanta()) <= 10e6;
}

// USBD_GS_CAN_Config_Request() / control_setup_request(): false = STALL
bool FakeGsUsbDevice::setupStage(uint8_t request, bool in, uint16_t value, uint16_t size)
{
    const int channels = static_cast<int>(_ch.size());
    if (_fw == Firmware::CandleLightF072)
    {
        const bool perDevice = request == BREQ_HOST_FORMAT || request == BREQ_DEVICE_CONFIG;
        if (!perDevice && value >= channels)
            return false;
        uint16_t len = 0;
        switch (request)
        {
        case BREQ_HOST_FORMAT: len = 4; break;
        case BREQ_BITTIMING: len = 20; break;
        case BREQ_MODE: len = 8; break;
        case BREQ_BT_CONST: len = 40; break;
        case BREQ_DEVICE_CONFIG: len = 12; break;
        case BREQ_TIMESTAMP: len = 4; break;
        case BREQ_IDENTIFY: len = 4; break;
        case BREQ_GET_STATE: len = 12; break;
        case BREQ_BUS_OFF_RECOVERY: len = 0; break;
        default:
            return false;  // no CONFIG_CANFD / TERMINATION / FILTER, BERR, user id, ELM
        }
        return size >= len;  // "if (req->wLength < len) goto out_fail;"
    }

    // Elmue
    if (request == ELM_REQ_GET_LAST_ERROR)
        return true;
    if (request >= ELM_REQ_FIRST)
        return false;  // needs the ElmüSoft protocol, which candle.c never enables
    const bool perDevice = request == BREQ_HOST_FORMAT || request == BREQ_DEVICE_CONFIG || request == BREQ_TIMESTAMP;
    if (!perDevice && value >= channels)
        return false;
    if (!in)
    {
        uint16_t minLen = 0;
        switch (request)
        {
        case BREQ_HOST_FORMAT: minLen = 4; break;
        case BREQ_IDENTIFY: minLen = 4; break;
        case BREQ_BITTIMING:
        case BREQ_DATA_BITTIMING: minLen = 20; break;
        case BREQ_MODE: minLen = 8; break;
        default:
            return false;  // FBK_InvalidCommand: BERR, user id, termination (no pins), filter, TDC
        }
        return size >= minLen;
    }
    switch (request)
    {
    case BREQ_BT_CONST:
    case BREQ_BT_CONST_EXT:
    case BREQ_DEVICE_CONFIG:
    case BREQ_TIMESTAMP:
    case BREQ_GET_STATE:
        return true;  // len = MIN(len, wLength): a shorter read is allowed
    default:
        return false;  // incl. GET_TERMINATION: FBK_UnsupportedFeature without pins
    }
}

bool FakeGsUsbDevice::inData(uint8_t request, uint16_t value, uint8_t *data, uint16_t size)
{
    std::array<uint32_t, 18> words{};
    int n = 0;
    const int ch = std::min<int>(value, static_cast<int>(_ch.size()) - 1);
    switch (request)
    {
    case BREQ_BT_CONST:
    case BREQ_BT_CONST_EXT:
    {
        const bool f072 = _fw == Firmware::CandleLightF072;
        words = {feature(), clockHz(),
                 1, f072 ? 16u : 256u, 1, f072 ? 8u : 128u, f072 ? 4u : 128u, 1, f072 ? 1024u : 512u, 1,
                 1, 32, 1, 16, 16, 1, 32, 1};
        n = request == BREQ_BT_CONST ? 10 : 18;
        break;
    }
    case BREQ_DEVICE_CONFIG:
    {
        // upstream: icount, sw_version 2, hw_version 1; Elmue: HAL version
        // bytes, icount, FIRMWARE_VERSION_BCD, 0x200
        uint8_t cfg[12] = {};
        cfg[3] = static_cast<uint8_t>(_ch.size() - 1);
        put32raw(cfg + 4, _fw == Firmware::CandleLightF072 ? 2u : 0x261001u);
        put32raw(cfg + 8, _fw == Firmware::CandleLightF072 ? 1u : 0x200u);
        std::memcpy(data, cfg, std::min<uint16_t>(size, 12));
        return true;
    }
    case BREQ_TIMESTAMP:
        words[0] = timestampUs();
        n = 1;
        break;
    case BREQ_GET_STATE:
    {
        const Channel &c = _ch[ch];
        words[0] = static_cast<uint32_t>(c.started ? c.state
                                                   : (_fw == Firmware::ElmueG431 ? State::Stopped : c.state));
        words[1] = c.rxErr;
        words[2] = c.txErr;
        n = 3;
        break;
    }
    case ELM_REQ_GET_LAST_ERROR:
        if (size >= 1)
            data[0] = 0;
        return true;
    default:
        return false;
    }
    uint8_t out[72];
    for (int i = 0; i < n; ++i)
        put32raw(out + 4 * i, words[static_cast<size_t>(i)]);
    std::memcpy(data, out, std::min<int>(size, 4 * n));
    return true;
}

// EP0_RxReady / control_setup_OUT_data(): refusals here are not reported
void FakeGsUsbDevice::dataStage(uint8_t request, uint16_t value, const uint8_t *data, uint16_t size)
{
    const int ch = std::min<int>(value, static_cast<int>(_ch.size()) - 1);
    Channel &c = _ch[ch];
    const auto ignore = [&] { _ignored.append(request); };

    switch (request)
    {
    case BREQ_HOST_FORMAT:
        if (_fw == Firmware::ElmueG431 && (size < 4 || get32(data) != 0xbeef))
            ignore();
        return;
    case BREQ_BITTIMING:
    case BREQ_DATA_BITTIMING:
    {
        const bool dataPhase = request == BREQ_DATA_BITTIMING;
        const Timing t{get32(data), get32(data + 4), get32(data + 8), get32(data + 12), get32(data + 16)};
        // Elmue refuses timings while open; upstream stores them for the next start
        if (!timingOk(dataPhase, t) || (_fw == Firmware::ElmueG431 && c.started))
            return ignore();
        (dataPhase ? c.data : c.nominal) = t;
        return;
    }
    case BREQ_MODE:
    {
        const uint32_t mode = get32(data);
        const uint32_t flags = get32(data + 4);
        if (mode == 0)
        {
            stopChannel(ch);
            return;
        }
        if (mode != 1)
            return ignore();
        if (_fw == Firmware::ElmueG431)
        {
            // START while open, FD without data bitrate, or no nominal bitrate
            if (c.started || ((flags & F_FD) && !c.data) || !c.nominal)
                return ignore();
        }
        startChannel(ch, flags);
        return;
    }
    case BREQ_BUS_OFF_RECOVERY:
        // can_check_bus_off_recovery_ok(): running, host recovery on, bus off
        if (!c.started || !(c.flags & F_BUS_OFF_RECOVERY) || c.state != State::BusOff)
            return ignore();
        c.txErr = c.rxErr = 0;
        c.state = State::ErrorActive;
        errorFrame(ch, CAN_ERR_RESTARTED, 0);
        return;
    default:
        return;  // IDENTIFY
    }
}

bool FakeGsUsbDevice::control(uint8_t request, uint8_t requesttype, uint16_t value, uint16_t /*index*/,
                              uint8_t *data, uint16_t size)
{
    std::lock_guard lock(_mutex);
    if (_unplugged)
        return false;
    const bool in = requesttype & 0x80;
    if (!setupStage(request, in, value, size))
    {
        _stalled.append(request);
        return false;
    }
    if (in)
        return inData(request, value, data, size);
    dataStage(request, value, data, size);
    _cond.notify_all();
    return true;  // the data stage is acknowledged whatever happened
}
