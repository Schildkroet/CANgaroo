#include "AiodeApi.hpp"
#include <cstring>
#include <sstream>

/* ---- bmRequestType constants ---- */
static constexpr uint8_t BRT_VENDOR_ITF_OUT = 0x41u; /* vendor | interface | host→device */
static constexpr uint8_t BRT_VENDOR_ITF_IN  = 0xC1u; /* vendor | interface | device→host */

static constexpr unsigned CTRL_TIMEOUT_MS = 1000u;

/* -------------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------------- */

AiodeApi::AiodeApi()
{
    libusb_init(&ctx_);
}

AiodeApi::~AiodeApi()
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

bool AiodeApi::setError(int rc, const char *context)
{
    std::ostringstream os;
    os << context << ": " << libusb_strerror(static_cast<libusb_error>(rc));
    last_error_ = os.str();
    return false;
}

/* -------------------------------------------------------------------------
 * open / close
 * ------------------------------------------------------------------------- */

bool AiodeApi::open(uint16_t vid, uint16_t pid)
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

    /* Find the AIO interface by bInterfaceProtocol == 0x02 */
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
                alt.bInterfaceProtocol == AIO_USB_ITF_PROTOCOL)
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
        last_error_ = "AIO interface (bInterfaceProtocol=0x02) not found in config descriptor";
        return false;
    }

    /* On Linux, detach any kernel driver holding this interface so libusb can
     * claim it.  We track this so close() can re-attach the driver explicitly. */
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

    last_error_.clear();
    return true;
}

void AiodeApi::close()
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
        dev_    = nullptr;
        ep_in_  = 0;
        ep_out_ = 0;
        itf_    = 0;
    }
}

/* -------------------------------------------------------------------------
 * Low-level control transfer helpers
 * ------------------------------------------------------------------------- */

bool AiodeApi::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
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
    if (rc < 0)
    {
        return setError(rc, "control OUT");
    }
    return true;
}

bool AiodeApi::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
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
    if (rc < 0)
    {
        return setError(rc, "control IN");
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Informational
 * ------------------------------------------------------------------------- */

bool AiodeApi::getCaps(aio_usb_caps_t &caps)
{
    std::memset(&caps, 0, sizeof(caps));
    return controlIn(AIO_USB_BREQ_DEVICE_CONFIG, 0u, &caps, sizeof(caps));
}

bool AiodeApi::getTimestamp(uint32_t &ts_ms)
{
    ts_ms = 0;
    return controlIn(AIO_USB_BREQ_TIMESTAMP, 0u, &ts_ms, sizeof(ts_ms));
}

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

bool AiodeApi::setHostFormat()
{
    aio_usb_host_config_t cfg;
    cfg.byte_order = 0x0000BEEFu; /* little-endian magic; firmware ignores value */
    return controlOut(AIO_USB_BREQ_HOST_FORMAT, 0u, &cfg, sizeof(cfg));
}

bool AiodeApi::identify()
{
    uint8_t dummy = 0;
    return controlOut(AIO_USB_BREQ_IDENTIFY, 0u, &dummy, 1u);
}

bool AiodeApi::configIo(uint8_t io, uint8_t mode, uint8_t default_state,
                        uint8_t flags, uint32_t auto_report_ms)
{
    aio_usb_io_config_t cfg{};
    cfg.mode           = mode;
    cfg.default_state  = default_state;
    cfg.flags          = flags;
    cfg.auto_report_ms = auto_report_ms;
    /* wValue low byte carries the I/O line index */
    return controlOut(AIO_USB_BREQ_IO_CONFIG, io, &cfg, sizeof(cfg));
}

/* -------------------------------------------------------------------------
 * Output control
 * ------------------------------------------------------------------------- */

bool AiodeApi::setOutputs(uint32_t mask, uint32_t values)
{
    aio_usb_io_set_t s{};
    s.mask   = mask;
    s.values = values;
    return controlOut(AIO_USB_BREQ_IO_SET, 0u, &s, sizeof(s));
}

bool AiodeApi::setOutputsBulk(uint32_t mask, uint32_t values)
{
    aio_usb_io_set_t s{};
    s.mask   = mask;
    s.values = values;

    int transferred = 0;
    unsigned char buf[sizeof(aio_usb_io_set_t)];
    std::memcpy(buf, &s, sizeof(s));

    int rc = libusb_bulk_transfer(dev_, ep_out_, buf, sizeof(buf),
                                   &transferred, CTRL_TIMEOUT_MS);
    if (rc != 0)
    {
        return setError(rc, "bulk OUT");
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

bool AiodeApi::readStatus(aio_usb_report_t &status)
{
    std::memset(&status, 0, sizeof(status));
    return controlIn(AIO_USB_BREQ_READ_STATUS, 0u, &status, sizeof(status));
}

bool AiodeApi::receiveReport(aio_usb_report_t &report, unsigned timeout_ms)
{
    int transferred = 0;
    unsigned char buf[sizeof(aio_usb_report_t)] = {};

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
    /* analog[] is sized to the device's channel count, so a report may be
     * shorter than aio_usb_report_t; missing entries stay 0. */
    if (transferred < static_cast<int>(AIO_USB_REPORT_HDR_SIZE))
    {
        last_error_ = "short bulk IN packet";
        return false;
    }
    std::memcpy(&report, buf, sizeof(report));
    return true;
}
