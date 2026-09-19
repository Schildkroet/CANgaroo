#include "CandeApi.hpp"
#include <cstddef>
#include <cstring>
#include <sstream>

static constexpr uint8_t  BRT_VENDOR_ITF_OUT = 0x41u;
static constexpr uint8_t  BRT_VENDOR_ITF_IN  = 0xC1u;
static constexpr unsigned CTRL_TIMEOUT_MS    = 1000u;

/* -------------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------------- */

CandeApi::CandeApi()
{
    libusb_init(&ctx_);
}

CandeApi::~CandeApi()
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

bool CandeApi::setError(int rc, const char *context)
{
    std::ostringstream os;
    os << context << ": " << libusb_strerror(static_cast<libusb_error>(rc));
    last_error_ = os.str();
    return false;
}

bool CandeApi::checkChannel(uint8_t ch)
{
    if (ch >= channel_count_)
    {
        std::ostringstream os;
        os << "channel " << static_cast<unsigned>(ch)
           << " out of range (device has "
           << static_cast<unsigned>(channel_count_) << " channel(s))";
        last_error_ = os.str();
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * open / close
 * ------------------------------------------------------------------------- */

bool CandeApi::open(uint16_t vid, uint16_t pid)
{
    if (dev_)
    {
        return true;
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

    /* Find the CAN interface: bInterfaceProtocol == 0xFF */
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
                alt.bInterfaceProtocol == GS_USB_ITF_PROTOCOL)
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
        last_error_ = "CAN interface (bInterfaceProtocol=0xFF) not found in config descriptor";
        return false;
    }

    /* On Linux, detach the gs_usb kernel driver so libusb can claim the
     * interface.  We track whether we detached it so close() can re-attach
     * it explicitly, restoring the canX SocketCAN interface. */
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

    /* Read device config now to populate channel_count_. */
    gs_device_config_t cfg{};
    if (!getDeviceConfig(cfg))
    {
        close();
        return false; /* last_error_ already set */
    }
    channel_count_ = static_cast<uint8_t>(cfg.icount + 1u);

    last_error_.clear();
    return true;
}

void CandeApi::close()
{
    if (dev_)
    {
        libusb_release_interface(dev_, itf_);
#ifndef _WIN32
        /* Explicitly re-attach the gs_usb kernel driver so the canX SocketCAN
         * interface reappears after this libusb session ends. */
        if (kernel_driver_detached_)
        {
            libusb_attach_kernel_driver(dev_, itf_);
            kernel_driver_detached_ = false;
        }
#endif
        libusb_close(dev_);
        dev_           = nullptr;
        ep_in_         = 0;
        ep_out_        = 0;
        itf_           = 0;
        channel_count_ = 0;
    }
}

/* -------------------------------------------------------------------------
 * Low-level control transfer helpers
 * ------------------------------------------------------------------------- */

bool CandeApi::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
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

bool CandeApi::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
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
        last_error_ = "control IN: request not supported by device (STALL)";
        return false;
    }
    if (rc < 0)
    {
        return setError(rc, "control IN");
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Device-level queries
 * ------------------------------------------------------------------------- */

bool CandeApi::getDeviceConfig(gs_device_config_t &cfg)
{
    std::memset(&cfg, 0, sizeof(cfg));
    return controlIn(GS_USB_BREQ_DEVICE_CONFIG, 0u, &cfg, sizeof(cfg));
}

bool CandeApi::getTimestamp(uint32_t &ts_us)
{
    ts_us = 0;
    return controlIn(GS_USB_BREQ_TIMESTAMP, 0u, &ts_us, sizeof(ts_us));
}

bool CandeApi::getBitTimingConst(uint8_t ch, gs_device_bt_const_t &bt)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    std::memset(&bt, 0, sizeof(bt));
    return controlIn(GS_USB_BREQ_BT_CONST, static_cast<uint16_t>(ch), &bt, sizeof(bt));
}

bool CandeApi::getBitTimingConstExt(uint8_t ch, gs_device_bt_const_ext_t &bt)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    std::memset(&bt, 0, sizeof(bt));
    return controlIn(GS_USB_BREQ_BT_CONST_EXT, static_cast<uint16_t>(ch), &bt, sizeof(bt));
}

bool CandeApi::getState(uint8_t ch, gs_device_state_t &state)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    std::memset(&state, 0, sizeof(state));
    return controlIn(GS_USB_BREQ_GET_STATE, static_cast<uint16_t>(ch), &state, sizeof(state));
}

bool CandeApi::setHostFormat()
{
    gs_host_config_t cfg;
    cfg.byte_order = 0x0000BEEFu;
    return controlOut(GS_USB_BREQ_HOST_FORMAT, 0u, &cfg, sizeof(cfg));
}

/* -------------------------------------------------------------------------
 * Channel-level configuration
 * ------------------------------------------------------------------------- */

bool CandeApi::setBitTiming(uint8_t ch, const gs_device_bittiming_t &bt)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    gs_device_bittiming_t tmp = bt;
    return controlOut(GS_USB_BREQ_BITTIMING, static_cast<uint16_t>(ch),
                      &tmp, sizeof(tmp));
}

bool CandeApi::setDataBitTiming(uint8_t ch, const gs_device_bittiming_t &bt)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    gs_device_bittiming_t tmp = bt;
    return controlOut(GS_USB_BREQ_DATA_BITTIMING, static_cast<uint16_t>(ch),
                      &tmp, sizeof(tmp));
}

bool CandeApi::setMode(uint8_t ch, uint32_t mode, uint32_t flags)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    gs_device_mode_t m;
    m.mode  = mode;
    m.flags = flags;
    return controlOut(GS_USB_BREQ_MODE, static_cast<uint16_t>(ch),
                      &m, sizeof(m));
}

bool CandeApi::identify(uint8_t ch)
{
    if (!checkChannel(ch))
    {
        return false;
    }
    uint8_t dummy = 0;
    return controlOut(GS_USB_BREQ_IDENTIFY, static_cast<uint16_t>(ch),
                      &dummy, 1u);
}

/* -------------------------------------------------------------------------
 * Bulk frame I/O
 * ------------------------------------------------------------------------- */

bool CandeApi::sendFrame(const gs_host_frame_t &frame)
{
    if (frame.channel >= channel_count_)
    {
        std::ostringstream os;
        os << "sendFrame: channel " << static_cast<unsigned>(frame.channel)
           << " out of range";
        last_error_ = os.str();
        return false;
    }

    /* Only the layout-specific part goes on the wire (no timestamp on OUT). */
    const bool fd  = (frame.flags & GS_FRAME_FLAG_FD) != 0u;
    const int  len = fd ? GS_HOST_FRAME_SIZE_FD : GS_HOST_FRAME_SIZE;

    unsigned char buf[sizeof(gs_host_frame_t)];
    std::memcpy(buf, &frame, sizeof(frame));

    int transferred = 0;
    int rc = libusb_bulk_transfer(dev_, ep_out_, buf, len,
                                   &transferred, CTRL_TIMEOUT_MS);
    if (rc != 0)
    {
        return setError(rc, "bulk OUT");
    }
    return true;
}

bool CandeApi::receiveFrame(gs_host_frame_t &frame, unsigned timeout_ms)
{
    /* Large enough for the biggest layout (FD + timestamp, 80 bytes).  Every
     * frame ends in a short packet, so one transfer returns exactly one frame. */
    unsigned char buf[sizeof(gs_host_frame_t)] = {};
    int transferred = 0;

    int rc = libusb_bulk_transfer(dev_, ep_in_, buf, sizeof(buf),
                                   &transferred,
                                   static_cast<unsigned int>(timeout_ms));
    if (rc == LIBUSB_ERROR_TIMEOUT)
    {
        last_error_ = "receive timeout";
        return false;
    }
    if (rc != 0)
    {
        return setError(rc, "bulk IN");
    }
    if (transferred < static_cast<int>(GS_HOST_FRAME_HDR_SIZE))
    {
        last_error_ = "short bulk IN packet";
        return false;
    }
    const bool fd = (buf[offsetof(gs_host_frame_t, flags)] & GS_FRAME_FLAG_FD) != 0u;
    if (transferred < static_cast<int>(fd ? GS_HOST_FRAME_SIZE_FD : GS_HOST_FRAME_SIZE))
    {
        last_error_ = "short bulk IN packet";
        return false;
    }
    std::memcpy(&frame, buf, sizeof(frame));
    return true;
}
