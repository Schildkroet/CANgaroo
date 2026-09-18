/*

  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

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

#include "AiodeApi.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include <QVector>

/* ---- bmRequestType constants (mirror LindeApi) ---- */
static constexpr uint8_t BRT_VENDOR_ITF_OUT = 0x41u; /* vendor | interface | host->device */
static constexpr uint8_t BRT_VENDOR_ITF_IN  = 0xC1u; /* vendor | interface | device->host */

static constexpr unsigned CTRL_TIMEOUT_MS = 1000u;

namespace
{
// True if @p device exposes the AIO vendor interface (subclass 0xFF, protocol 0x02).
bool hasAioInterface(libusb_device *device)
{
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(device, &cfg) != 0)
        return false;

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; ++i)
    {
        const libusb_interface &ifc = cfg->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !found; ++a)
        {
            const libusb_interface_descriptor &alt = ifc.altsetting[a];
            if (alt.bInterfaceClass    == LIBUSB_CLASS_VENDOR_SPEC &&
                alt.bInterfaceSubClass == 0xFFu &&
                alt.bInterfaceProtocol == AIO_USB_ITF_PROTOCOL)
            {
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}
} // namespace

/* -------------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------------- */

AiodeApi::AiodeApi(QObject *parent)
    : GpioProvider(parent)
{
    // gpioUpdated() is emitted from the poll thread and delivered via a queued
    // connection, which copies the arguments — register the container type.
    qRegisterMetaType<QVector<uint16_t>>("QVector<uint16_t>");
}

AiodeApi::~AiodeApi()
{
    close();
}

/* -------------------------------------------------------------------------
 * Discovery
 * ------------------------------------------------------------------------- */

QList<AiodeApi *> AiodeApi::scan(QObject *parent)
{
    QList<AiodeApi *> result;

    libusb_context *enumCtx = nullptr;
    if (libusb_init(&enumCtx) != 0)
        return result;

    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(enumCtx, &list);
    if (cnt < 0)
    {
        libusb_exit(enumCtx);
        return result;
    }

    struct Match { uint8_t bus; uint8_t addr; };
    QList<Match> matches;
    for (ssize_t i = 0; i < cnt; ++i)
    {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != AIO_USB_VID || desc.idProduct != AIO_USB_PID)
            continue;
        if (!hasAioInterface(list[i]))
            continue;
        matches.append({libusb_get_bus_number(list[i]),
                        libusb_get_device_address(list[i])});
    }
    libusb_free_device_list(list, 1);
    libusb_exit(enumCtx);

    int idx = 0;
    for (const Match &m : matches)
    {
        auto *api = new AiodeApi(parent);
        if (api->openByAddress(m.bus, m.addr, idx))
        {
            ++idx;
            result.append(api);
        }
        else
        {
            delete api;
        }
    }
    return result;
}

/* -------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------- */

bool AiodeApi::openByAddress(uint8_t bus, uint8_t addr, int index)
{
    if (libusb_init(&ctx_) != 0)
    {
        ctx_ = nullptr;
        last_error_ = "libusb_init failed";
        return false;
    }

    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx_, &list);
    if (cnt < 0)
    {
        setError(static_cast<int>(cnt), "libusb_get_device_list");
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    libusb_device *device = nullptr;
    for (ssize_t i = 0; i < cnt; ++i)
    {
        if (libusb_get_bus_number(list[i]) == bus &&
            libusb_get_device_address(list[i]) == addr)
        {
            device = libusb_ref_device(list[i]);
            break;
        }
    }

    bool ok = device && openDevice(device, index);
    if (device)
        libusb_unref_device(device);
    libusb_free_device_list(list, 1);

    if (!ok)
    {
        // close() is safe here even though the poll thread was never started:
        // stopPolling() is a no-op while pollRunning_ is false.
        if (dev_)
            close();
        else if (ctx_)
        {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
        return false;
    }
    return true;
}

bool AiodeApi::openDevice(libusb_device *device, int index)
{
    int rc = libusb_open(device, &dev_);
    if (rc != 0)
    {
        dev_ = nullptr;
        return setError(rc, "libusb_open");
    }

    libusb_config_descriptor *cfg = nullptr;
    rc = libusb_get_active_config_descriptor(device, &cfg);
    if (rc != 0)
        return setError(rc, "libusb_get_active_config_descriptor");

    bool found_itf = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found_itf; ++i)
    {
        const libusb_interface &ifc = cfg->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !found_itf; ++a)
        {
            const libusb_interface_descriptor &alt = ifc.altsetting[a];
            if (alt.bInterfaceClass    == LIBUSB_CLASS_VENDOR_SPEC &&
                alt.bInterfaceSubClass == 0xFFu &&
                alt.bInterfaceProtocol == AIO_USB_ITF_PROTOCOL)
            {
                itf_ = alt.bInterfaceNumber;
                for (uint8_t e = 0; e < alt.bNumEndpoints; ++e)
                {
                    const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                    if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                        ep_in_ = ep.bEndpointAddress;
                    else
                        ep_out_ = ep.bEndpointAddress;
                }
                found_itf = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    if (!found_itf)
    {
        last_error_ = "AIO interface (bInterfaceProtocol=0x02) not found";
        return false;
    }

#ifndef _WIN32
    if (libusb_kernel_driver_active(dev_, itf_) == 1)
    {
        rc = libusb_detach_kernel_driver(dev_, itf_);
        if (rc != 0)
            return setError(rc, "libusb_detach_kernel_driver");
        kernel_driver_detached_ = true;
    }
#endif

    rc = libusb_claim_interface(dev_, itf_);
    if (rc != 0)
        return setError(rc, "libusb_claim_interface");

    setHostFormat();

    if (!getCaps(caps_))
        return false;

    name_ = QStringLiteral("Aiode Device %1").arg(index + 1);
    last_error_.clear();
    return true;
}

void AiodeApi::close()
{
    stopPolling();

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
        dev_ = nullptr;
    }
    if (ctx_)
    {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
    ep_in_ = ep_out_ = itf_ = 0;
}

/* -------------------------------------------------------------------------
 * Low-level control transfers
 * ------------------------------------------------------------------------- */

bool AiodeApi::setError(int rc, const char *context)
{
    std::ostringstream os;
    os << context << ": " << libusb_strerror(static_cast<libusb_error>(rc));
    last_error_ = os.str();
    return false;
}

bool AiodeApi::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    if (!dev_)
        return false;
    int rc = libusb_control_transfer(dev_, BRT_VENDOR_ITF_OUT, breq, wValue,
                                     static_cast<uint16_t>(itf_),
                                     static_cast<unsigned char *>(data), len,
                                     CTRL_TIMEOUT_MS);
    return (rc < 0) ? setError(rc, "control OUT") : true;
}

bool AiodeApi::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    if (!dev_)
        return false;
    int rc = libusb_control_transfer(dev_, BRT_VENDOR_ITF_IN, breq, wValue,
                                     static_cast<uint16_t>(itf_),
                                     static_cast<unsigned char *>(data), len,
                                     CTRL_TIMEOUT_MS);
    return (rc < 0) ? setError(rc, "control IN") : true;
}

bool AiodeApi::setHostFormat()
{
    aio_usb_host_config_t cfg{};
    cfg.byte_order = 0x0000beefu; /* little-endian marker; firmware ignores value */
    return controlOut(AIO_USB_BREQ_HOST_FORMAT, 0, &cfg, sizeof(cfg));
}

bool AiodeApi::getCaps(aio_usb_caps_t &caps)
{
    std::memset(&caps, 0, sizeof(caps));
    return controlIn(AIO_USB_BREQ_DEVICE_CONFIG, 0, &caps, sizeof(caps));
}

bool AiodeApi::readStatus(aio_usb_report_t &report)
{
    std::memset(&report, 0, sizeof(report));
    return controlIn(AIO_USB_BREQ_READ_STATUS, 0, &report, sizeof(report));
}

/* -------------------------------------------------------------------------
 * GpioProvider interface
 * ------------------------------------------------------------------------- */

int AiodeApi::digitalPinCount() const
{
    return std::min<int>(16, caps_.io_count);
}

int AiodeApi::analogPinCount() const
{
    if (!(caps_.features & AIO_USB_FEATURE_ANALOG))
        return 0;
    return std::min<int>(16, caps_.analog_count);
}

void AiodeApi::setConfig(bool enable, uint16_t cycleMs, uint16_t dirMask)
{
    // Stop the poll thread first so reconfiguration owns the USB exclusively.
    stopPolling();

    dirMask_ = dirMask;
    cycleMs_ = std::max<uint32_t>(5u, cycleMs);

    if (!enable)
        return;

    QMutexLocker lock(&usbMutex_);
    const int pins = digitalPinCount();
    for (int line = 0; line < pins; ++line)
    {
        aio_usb_io_config_t cfg{};
        cfg.mode          = (dirMask >> line) & 1u ? AIO_USB_IO_MODE_OUTPUT
                                                   : AIO_USB_IO_MODE_INPUT;
        cfg.default_state = 0;
        cfg.flags         = 0;
        cfg.auto_report_ms = 0; /* host polls READ_STATUS instead */
        controlOut(AIO_USB_BREQ_IO_CONFIG, static_cast<uint16_t>(line), &cfg, sizeof(cfg));
    }

    // Apply the current desired output levels for the freshly-configured outputs.
    aio_usb_io_set_t set{};
    set.mask   = dirMask;
    set.values = static_cast<uint32_t>(outMask_.load() & dirMask);
    controlOut(AIO_USB_BREQ_IO_SET, 0, &set, sizeof(set));
    lock.unlock();

    startPolling();
}

void AiodeApi::setOutput(uint16_t outputMask)
{
    outMask_ = outputMask;

    const uint16_t dir = dirMask_.load();
    aio_usb_io_set_t set{};
    set.mask   = dir;
    set.values = static_cast<uint32_t>(outputMask & dir);

    QMutexLocker lock(&usbMutex_);
    controlOut(AIO_USB_BREQ_IO_SET, 0, &set, sizeof(set));
}

/* -------------------------------------------------------------------------
 * Background polling
 * ------------------------------------------------------------------------- */

void AiodeApi::startPolling()
{
    if (pollRunning_.exchange(true))
        return; /* already running */
    pollThread_ = std::thread(&AiodeApi::pollLoop, this);
}

void AiodeApi::stopPolling()
{
    if (!pollRunning_.exchange(false))
        return;
    if (pollThread_.joinable())
        pollThread_.join();
}

void AiodeApi::pollLoop()
{
    while (pollRunning_.load())
    {
        aio_usb_report_t report{};
        bool ok = false;
        {
            QMutexLocker lock(&usbMutex_);
            ok = readStatus(report);
        }

        if (ok)
        {
            const int analogCount = analogPinCount();
            QVector<uint16_t> analog(analogCount);
            for (int i = 0; i < analogCount; ++i)
                analog[i] = report.analog[i];

            emit gpioUpdated(static_cast<uint16_t>(report.io_states & 0xFFFFu), analog);
        }

        // Sleep in small slices so a disable request stops us promptly.
        uint32_t remaining = cycleMs_.load();
        while (remaining > 0 && pollRunning_.load())
        {
            const uint32_t slice = std::min<uint32_t>(remaining, 20u);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    }
}
