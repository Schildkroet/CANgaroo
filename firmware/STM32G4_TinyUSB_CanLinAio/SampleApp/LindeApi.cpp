#include "LindeApi.hpp"
#include <chrono>
#include <cstring>
#include <sstream>

/* ---- bmRequestType constants ---- */
static constexpr uint8_t BRT_VENDOR_ITF_OUT = 0x41u; /* vendor | interface | host→device */
static constexpr uint8_t BRT_VENDOR_ITF_IN  = 0xC1u; /* vendor | interface | device→host */

static constexpr unsigned CTRL_TIMEOUT_MS = 1000u;

/* -------------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------------- */

LindeApi::LindeApi(uint8_t channel)
    : channel_(channel)
{
    libusb_init(&ctx_);
}

LindeApi::~LindeApi()
{
    close();
    if (ctx_)
    {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

/* -------------------------------------------------------------------------
 * Error helper
 * ------------------------------------------------------------------------- */

bool LindeApi::setError(int rc, const char *context)
{
    std::ostringstream os;
    os << context << ": " << libusb_strerror(static_cast<libusb_error>(rc));
    last_error_ = os.str();
    return false;
}

bool LindeApi::checkChannel() const
{
    if (channel_ >= channel_count_)
    {
        /* channel_count_ is set in open(); this guard fires only if the
         * constructor channel index exceeds what the device reports. */
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * open / close
 * ------------------------------------------------------------------------- */

bool LindeApi::open(uint16_t vid, uint16_t pid)
{
    if (dev_)
    {
        return true; /* already open */
    }

    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx_, &list);
    if (cnt < 0)
    {
        return setError(static_cast<int>(cnt), "libusb_get_device_list");
    }

    libusb_device *found = nullptr;
    for (ssize_t i = 0; i < cnt; i++)
    {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
        {
            continue;
        }
        if (desc.idVendor == vid && desc.idProduct == pid)
        {
            found = list[i];
            break;
        }
    }

    if (!found)
    {
        libusb_free_device_list(list, 1);
        last_error_ = "device not found (VID/PID mismatch)";
        return false;
    }

    int rc = libusb_open(found, &dev_);
    libusb_free_device_list(list, 1);
    if (rc != 0)
    {
        dev_ = nullptr;
        return setError(rc, "libusb_open");
    }

    /* Find the LIN interface by bInterfaceProtocol == 0x01 */
    libusb_config_descriptor *cfg_desc = nullptr;
    rc = libusb_get_active_config_descriptor(libusb_get_device(dev_), &cfg_desc);
    if (rc != 0)
    {
        close();
        return setError(rc, "libusb_get_active_config_descriptor");
    }

    bool found_itf = false;
    for (uint8_t i = 0; i < cfg_desc->bNumInterfaces && !found_itf; i++)
    {
        const libusb_interface &ifc = cfg_desc->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !found_itf; a++)
        {
            const libusb_interface_descriptor &alt = ifc.altsetting[a];
            if (alt.bInterfaceClass    == LIBUSB_CLASS_VENDOR_SPEC &&
                alt.bInterfaceSubClass == 0xFFu &&
                alt.bInterfaceProtocol == LIN_USB_ITF_PROTOCOL)
            {
                itf_ = alt.bInterfaceNumber;
                for (uint8_t e = 0; e < alt.bNumEndpoints; e++)
                {
                    const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                    if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                        LIBUSB_ENDPOINT_IN)
                    {
                        ep_in_ = ep.bEndpointAddress;
                    }
                    else
                    {
                        ep_out_ = ep.bEndpointAddress;
                    }
                }
                found_itf = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg_desc);

    if (!found_itf)
    {
        close();
        last_error_ = "LIN interface (bInterfaceProtocol=0x01) not found in config descriptor";
        return false;
    }

    /* On Linux, detach any kernel driver holding this interface so libusb can
     * claim it.  We track this so close() can re-attach the driver explicitly,
     * restoring any kernel-managed interface (e.g. SocketCAN canX for gs_usb). */
#ifndef _WIN32
    if (libusb_kernel_driver_active(dev_, itf_) == 1)
    {
        rc = libusb_detach_kernel_driver(dev_, itf_);
        if (rc != 0)
        {
            close();
            return setError(rc, "libusb_detach_kernel_driver");
        }
        kernel_driver_detached_ = true;
    }
#endif

    rc = libusb_claim_interface(dev_, itf_);
    if (rc != 0)
    {
        close();
        return setError(rc, "libusb_claim_interface");
    }

    /* Query device config to populate channel_count_ and validate channel_. */
    lin_usb_device_config_t dcfg{};
    if (!getDeviceConfig(dcfg))
    {
        close();
        return false;
    }
    channel_count_ = static_cast<uint8_t>(dcfg.icount + 1u);

    if (channel_ >= channel_count_)
    {
        std::ostringstream os;
        os << "channel " << static_cast<unsigned>(channel_)
           << " out of range (device has "
           << static_cast<unsigned>(channel_count_) << " channel(s))";
        last_error_ = os.str();
        close();
        return false;
    }

    last_error_.clear();
    return true;
}

void LindeApi::close()
{
    if (dev_)
    {
        libusb_release_interface(dev_, itf_);
#ifndef _WIN32
        if (kernel_driver_detached_)
        {
            libusb_attach_kernel_driver(dev_, itf_);
            kernel_driver_detached_ = false;
        }
#endif
        libusb_close(dev_);
        dev_           = nullptr;
        pending_.clear();
        ep_in_         = 0;
        ep_out_        = 0;
        itf_           = 0;
        channel_count_ = 0;
    }
}

/* -------------------------------------------------------------------------
 * Low-level control transfer helpers
 * ------------------------------------------------------------------------- */

bool LindeApi::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    int rc = libusb_control_transfer(
        dev_,
        BRT_VENDOR_ITF_OUT,
        breq,
        wValue,
        static_cast<uint16_t>(itf_),
        static_cast<unsigned char *>(data),
        len,
        CTRL_TIMEOUT_MS);
    if (rc == LIBUSB_ERROR_PIPE)
    {
        last_error_ = "control OUT: request rejected by device (STALL)";
        return false;
    }
    if (rc < 0)
    {
        return setError(rc, "control OUT");
    }
    return true;
}

bool LindeApi::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    int rc = libusb_control_transfer(
        dev_,
        BRT_VENDOR_ITF_IN,
        breq,
        wValue,
        static_cast<uint16_t>(itf_),
        static_cast<unsigned char *>(data),
        len,
        CTRL_TIMEOUT_MS);
    if (rc == LIBUSB_ERROR_PIPE)
    {
        last_error_ = "control IN: request rejected by device (STALL)";
        return false;
    }
    if (rc < 0)
    {
        return setError(rc, "control IN");
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Informational
 * ------------------------------------------------------------------------- */

bool LindeApi::getDeviceConfig(lin_usb_device_config_t &cfg)
{
    std::memset(&cfg, 0, sizeof(cfg));
    return controlIn(LIN_USB_BREQ_DEVICE_CONFIG,
                     static_cast<uint16_t>(channel_),
                     &cfg, sizeof(cfg));
}

bool LindeApi::getTimestamp(uint32_t &ts_ms)
{
    ts_ms = 0;
    return controlIn(LIN_USB_BREQ_TIMESTAMP,
                     static_cast<uint16_t>(channel_),
                     &ts_ms, sizeof(ts_ms));
}

bool LindeApi::getBusState(lin_usb_bus_state_t &state)
{
    if (!checkChannel()) { last_error_ = "getBusState: channel out of range"; return false; }
    std::memset(&state, 0, sizeof(state));
    return controlIn(LIN_USB_BREQ_BUS_STATE,
                     static_cast<uint16_t>(channel_),
                     &state, sizeof(state));
}

/* -------------------------------------------------------------------------
 * Channel configuration
 * ------------------------------------------------------------------------- */

bool LindeApi::setHostFormat()
{
    lin_usb_host_config_t cfg;
    cfg.byte_order = 0xEFu; /* little-endian magic; firmware ignores value */
    return controlOut(LIN_USB_BREQ_HOST_FORMAT,
                      static_cast<uint16_t>(channel_),
                      &cfg, sizeof(cfg));
}

bool LindeApi::setBusConfig(const lin_usb_bus_config_t &cfg)
{
    if (!checkChannel()) { last_error_ = "setBusConfig: channel out of range"; return false; }
    lin_usb_bus_config_t tmp = cfg;
    return controlOut(LIN_USB_BREQ_BAUDRATE,
                      static_cast<uint16_t>(channel_),
                      &tmp, sizeof(tmp));
}

bool LindeApi::setMode(uint8_t mode)
{
    if (!checkChannel()) { last_error_ = "setMode: channel out of range"; return false; }
    lin_usb_mode_t m{};
    m.mode = mode;
    return controlOut(LIN_USB_BREQ_MODE,
                      static_cast<uint16_t>(channel_),
                      &m, sizeof(m));
}

bool LindeApi::identify()
{
    uint8_t dummy = 0;
    return controlOut(LIN_USB_BREQ_IDENTIFY,
                      static_cast<uint16_t>(channel_),
                      &dummy, 1u);
}

/* -------------------------------------------------------------------------
 * Schedule management
 * ------------------------------------------------------------------------- */

bool LindeApi::uploadScheduleEntry(uint8_t table_id, uint8_t slot,
                                    const lin_usb_schedule_entry_t &entry)
{
    if (!checkChannel()) { last_error_ = "uploadScheduleEntry: channel out of range"; return false; }
    lin_usb_schedule_entry_t tmp = entry;
    tmp.table_id = table_id;
    /* wValue: slot index in high byte, channel in low byte */
    uint16_t wval = static_cast<uint16_t>((slot << 8) | channel_);
    return controlOut(LIN_USB_BREQ_SCHEDULE, wval, &tmp, sizeof(tmp));
}

bool LindeApi::updateFrameConfig(const lin_usb_schedule_entry_t &entry)
{
    lin_usb_schedule_entry_t tmp = entry;
    return controlOut(LIN_USB_BREQ_FRAME_CONFIG,
                      static_cast<uint16_t>(channel_),
                      &tmp, sizeof(tmp));
}

bool LindeApi::scheduleStart(uint8_t table_id, uint8_t entry_count)
{
    lin_usb_mode_t m{};
    m.mode         = LIN_USB_MODE_START;
    m.table_id     = table_id;
    m.entry_count  = entry_count;
    return controlOut(LIN_USB_BREQ_MODE,
                      static_cast<uint16_t>(channel_),
                      &m, sizeof(m));
}

bool LindeApi::scheduleStop()
{
    lin_usb_mode_t m{};
    m.mode = LIN_USB_MODE_STOP;
    return controlOut(LIN_USB_BREQ_MODE,
                      static_cast<uint16_t>(channel_),
                      &m, sizeof(m));
}

bool LindeApi::schedulePause()
{
    lin_usb_mode_t m{};
    m.mode = LIN_USB_MODE_PAUSE;
    return controlOut(LIN_USB_BREQ_MODE,
                      static_cast<uint16_t>(channel_),
                      &m, sizeof(m));
}

/* -------------------------------------------------------------------------
 * Sleep / wakeup
 * ------------------------------------------------------------------------- */

bool LindeApi::goToSleep()
{
    lin_usb_sleep_wakeup_t cmd{};
    cmd.command = 0u;
    return controlOut(LIN_USB_BREQ_SLEEP_WAKEUP,
                      static_cast<uint16_t>(channel_),
                      &cmd, sizeof(cmd));
}

bool LindeApi::wakeup()
{
    lin_usb_sleep_wakeup_t cmd{};
    cmd.command = 1u;
    return controlOut(LIN_USB_BREQ_SLEEP_WAKEUP,
                      static_cast<uint16_t>(channel_),
                      &cmd, sizeof(cmd));
}

/* -------------------------------------------------------------------------
 * Bulk frame I/O (self-scheduling device)
 * ------------------------------------------------------------------------- */

bool LindeApi::readIn(lin_usb_host_frame_t &frame, unsigned timeout_ms)
{
    int transferred = 0;
    unsigned char buf[sizeof(lin_usb_host_frame_t)];

    int rc = libusb_bulk_transfer(dev_, ep_in_, buf, sizeof(buf),
                                   &transferred, static_cast<unsigned int>(timeout_ms));
    if (rc == LIBUSB_ERROR_TIMEOUT)
    {
        last_error_ = "receive timeout";
        return false;
    }
    if (rc != 0)
    {
        return setError(rc, "bulk IN");
    }
    if (transferred != static_cast<int>(sizeof(lin_usb_host_frame_t)))
    {
        last_error_ = "short bulk IN packet";
        return false;
    }
    std::memcpy(&frame, buf, sizeof(frame));
    return true;
}

/* Milliseconds left until `deadline`, at least 1 so libusb never gets 0
 * (which would mean "wait forever"). */
static unsigned msLeft(std::chrono::steady_clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return left > 1 ? static_cast<unsigned>(left) : 1u;
}

bool LindeApi::setFrame(const lin_usb_host_frame_t &frame, unsigned ack_timeout_ms)
{
    if (!checkChannel()) { last_error_ = "setFrame: channel out of range"; return false; }

    lin_usb_host_frame_t tmp = frame;
    tmp.channel = channel_;

    int transferred = 0;
    /* libusb bulk transfer takes a non-const pointer */
    unsigned char buf[sizeof(lin_usb_host_frame_t)];
    std::memcpy(buf, &tmp, sizeof(tmp));

    int rc = libusb_bulk_transfer(dev_, ep_out_, buf, sizeof(buf),
                                   &transferred, CTRL_TIMEOUT_MS);
    if (rc != 0)
    {
        return setError(rc, "bulk OUT");
    }

    /* Wait for the ACK; queue any bus events that arrive first. */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ack_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        lin_usb_host_frame_t in{};
        if (!readIn(in, msLeft(deadline)))
        {
            if (last_error_ == "receive timeout")
            {
                break;
            }
            return false;
        }
        if (in.echo_id != LIN_USB_ECHO_ID_SET_DATA_ACK)
        {
            pending_.push_back(in);
            continue;
        }
        if (in.flags & LIN_USB_FRAME_FLAG_ERROR)
        {
            std::ostringstream os;
            os << "setFrame: device rejected id 0x" << std::hex
               << static_cast<unsigned>(tmp.lin_id)
               << " (not a publisher in the running schedule, or invalid dlc)";
            last_error_ = os.str();
            return false;
        }
        return true;
    }

    last_error_ = "setFrame: no ACK from device";
    return false;
}

bool LindeApi::getFrame(lin_usb_host_frame_t &frame, unsigned timeout_ms)
{
    if (!pending_.empty())
    {
        frame = pending_.front();
        pending_.pop_front();
        return true;
    }

    /* Skip stray set-data ACKs (e.g. from a setFrame() that timed out). */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    do
    {
        if (!readIn(frame, msLeft(deadline)))
        {
            return false;
        }
    } while (frame.echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK &&
             std::chrono::steady_clock::now() < deadline);

    if (frame.echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK)
    {
        last_error_ = "receive timeout";
        return false;
    }
    return true;
}
