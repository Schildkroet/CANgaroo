#include "LindeSharedDevice.h"

#include "core/Log.h"

#include <QMutexLocker>
#include <QDeadlineTimer>
#include <QDateTime>

#include <cstring>

LindeSharedDevice::~LindeSharedDevice()
{
    stopReader();
    close(); // also frees ctx
}

bool LindeSharedDevice::open()
{
    // Always create a fresh context so re-open after close works cleanly.
    if (ctx)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }
    libusb_init(&ctx);

    libusb_device **list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0)
    {
        setLastError("libusb_get_device_list failed");
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    libusb_device *found = nullptr;
    for (ssize_t i = 0; i < cnt; i++)
    {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor == LIN_USB_VID && desc.idProduct == LIN_USB_PID)
        {
            found = list[i];
            break;
        }
    }

    if (!found)
    {
        setLastError("device not found (VID/PID mismatch)");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    int rc = libusb_open(found, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0)
    {
        lastError = libusb_strerror(static_cast<libusb_error>(rc));
        if (rc == LIBUSB_ERROR_ACCESS)
            log_warning(QStringLiteral("LindeAPI: cannot open USB device (VID 0x1d50 / PID 0x606f): "
                                       "permission denied. Add a udev rule or run as root.\n"));
        handle = nullptr;
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    // Find the LIN interface by bInterfaceProtocol == LIN_USB_ITF_PROTOCOL.
    libusb_config_descriptor *cfg_desc = nullptr;
    rc = libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg_desc);
    if (rc != 0)
    {
        lastError = libusb_strerror(static_cast<libusb_error>(rc));
        close();
        return false;
    }

    bool foundItf = false;
    for (uint8_t i = 0; i < cfg_desc->bNumInterfaces && !foundItf; i++)
    {
        const libusb_interface &ifc = cfg_desc->interface[i];
        for (int a = 0; a < ifc.num_altsetting && !foundItf; a++)
        {
            const libusb_interface_descriptor &alt = ifc.altsetting[a];
            if (alt.bInterfaceClass    == LIBUSB_CLASS_VENDOR_SPEC &&
                alt.bInterfaceSubClass == 0xFFu &&
                alt.bInterfaceProtocol == LIN_USB_ITF_PROTOCOL)
            {
                itf = alt.bInterfaceNumber;
                for (uint8_t e = 0; e < alt.bNumEndpoints; e++)
                {
                    const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                    if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                        ep_in = ep.bEndpointAddress;
                    else
                        ep_out = ep.bEndpointAddress;
                }
                foundItf = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg_desc);

    if (!foundItf)
    {
        setLastError("LIN interface (bInterfaceProtocol=0x01) not found");
        close();
        return false;
    }

#ifndef _WIN32
    if (libusb_kernel_driver_active(handle, itf) == 1)
    {
        rc = libusb_detach_kernel_driver(handle, itf);
        if (rc != 0)
        {
            setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
            close();
            return false;
        }
    }
#endif

    rc = libusb_claim_interface(handle, itf);
    if (rc != 0)
    {
        setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
        close();
        return false;
    }

    lin_usb_device_config_t dcfg{};
    if (!getDeviceConfig(dcfg))
    {
        close();
        return false;
    }
    channelCount = static_cast<uint8_t>(dcfg.icount + 1u);
    features     = dcfg.features;

    setLastError(std::string());
    return true;
}

void LindeSharedDevice::close()
{
    if (handle)
    {
        libusb_release_interface(handle, itf);
#ifndef _WIN32
        libusb_attach_kernel_driver(handle, itf);
#endif
        libusb_close(handle);
        handle       = nullptr;
        ep_in        = 0;
        ep_out       = 0;
        itf          = 0;
        channelCount = 0;
        features     = 0;
    }
    if (ctx)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }
}

// ---- Reader thread ----

void LindeSharedDevice::startReader()
{
    readerRunning.store(true, std::memory_order_relaxed);
    readerThread = std::thread([this]()
    {
        while (readerRunning.load(std::memory_order_relaxed))
        {
            lin_usb_host_frame_t frame{};
            int transferred = 0;
            int rc = libusb_bulk_transfer(handle, ep_in,
                                          reinterpret_cast<unsigned char *>(&frame),
                                          static_cast<int>(sizeof(frame)),
                                          &transferred,
                                          BULK_TIMEOUT_MS);
            if (rc == LIBUSB_ERROR_TIMEOUT)
                continue;
            if (rc != 0 || transferred != static_cast<int>(sizeof(frame)))
                continue;

            const uint8_t ch = frame.channel;
            if (ch >= MAX_CHANNELS)
            {
                log_error(QStringLiteral("LindeAPI: received frame for invalid channel %1")
                              .arg(static_cast<int>(ch)));
                continue;
            }

            QMutexLocker lock(&queueMutex);
            rxQueues[ch].append(frame);
            queueCond.wakeAll();
        }
    });
}

void LindeSharedDevice::stopReader()
{
    readerRunning.store(false, std::memory_order_relaxed);
    {
        // Wake any BusListener thread blocked in readFrame() so it can observe
        // the stopped state instead of waiting out its full deadline.
        QMutexLocker lock(&queueMutex);
        queueCond.wakeAll();
    }
    if (readerThread.joinable())
        readerThread.join();
    QMutexLocker lock(&queueMutex);
    for (auto &q : rxQueues)
        q.clear();
}

void LindeSharedDevice::resetTimestampEpoch()
{
    uint32_t ts_ms = 0;
    // Use channel 0 to probe the device timestamp.
    bool valid = getTimestamp(0, ts_ms);
    QMutexLocker lock(&timestampMutex);
    deviceTicksStart_ms = ts_ms;
    hostOffsetStart_ms  = QDateTime::currentMSecsSinceEpoch();
    timestampValid      = valid && (features & LIN_USB_FEATURE_TIMESTAMP);
}

int64_t LindeSharedDevice::deviceTimestampToHostMs(uint32_t ts_ms)
{
    QMutexLocker lock(&timestampMutex);
    if (!timestampValid)
        return QDateTime::currentMSecsSinceEpoch();
    const int64_t delta = static_cast<int64_t>(ts_ms) - static_cast<int64_t>(deviceTicksStart_ms);
    return hostOffsetStart_ms + delta;
}

bool LindeSharedDevice::readFrame(uint8_t channel, lin_usb_host_frame_t &frame, unsigned timeout_ms)
{
    QMutexLocker lock(&queueMutex);
    const QDeadlineTimer deadline(timeout_ms);
    while (rxQueues[channel].isEmpty())
    {
        // Bail out promptly if the reader has been stopped (e.g. on close).
        if (!readerRunning.load(std::memory_order_relaxed))
            return false;
        if (!queueCond.wait(&queueMutex, deadline))
            return false;
    }
    frame = rxQueues[channel].takeFirst();
    return true;
}

bool LindeSharedDevice::sendFrame(const lin_usb_host_frame_t &frame)
{
    QMutexLocker lock(&writeMutex);
    lin_usb_host_frame_t tmp = frame;
    int transferred = 0;
    int rc = libusb_bulk_transfer(handle, ep_out,
                                  reinterpret_cast<unsigned char *>(&tmp),
                                  static_cast<int>(sizeof(tmp)),
                                  &transferred,
                                  CTRL_TIMEOUT_MS);
    if (rc != 0)
    {
        setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
        return false;
    }
    return true;
}

// ---- Low-level control transfer helpers ----

bool LindeSharedDevice::controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    QMutexLocker tx(&controlMutex);
    int rc = libusb_control_transfer(handle, BRT_VENDOR_ITF_OUT, breq,
                                     wValue, static_cast<uint16_t>(itf),
                                     static_cast<unsigned char *>(data), len,
                                     CTRL_TIMEOUT_MS);
    if (rc < 0)
    {
        setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
        return false;
    }
    return true;
}

bool LindeSharedDevice::controlIn(uint8_t breq, uint16_t wValue, void *data, uint16_t len)
{
    QMutexLocker tx(&controlMutex);
    int rc = libusb_control_transfer(handle, BRT_VENDOR_ITF_IN, breq,
                                     wValue, static_cast<uint16_t>(itf),
                                     static_cast<unsigned char *>(data), len,
                                     CTRL_TIMEOUT_MS);
    if (rc < 0)
    {
        setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
        return false;
    }
    return true;
}

void LindeSharedDevice::setLastError(const std::string &err)
{
    QMutexLocker lock(&errorMutex);
    lastError = err;
}

std::string LindeSharedDevice::getLastError() const
{
    QMutexLocker lock(&errorMutex);
    return lastError;
}

// ---- Per-channel protocol helpers ----

bool LindeSharedDevice::setHostFormat(uint8_t channel)
{
    lin_usb_host_config_t cfg{};
    cfg.byte_order = 0xEFu; /* little-endian magic; firmware ignores value */
    return controlOut(LIN_USB_BREQ_HOST_FORMAT, static_cast<uint16_t>(channel),
                      &cfg, sizeof(cfg));
}

bool LindeSharedDevice::getDeviceConfig(lin_usb_device_config_t &cfg)
{
    std::memset(&cfg, 0, sizeof(cfg));
    return controlIn(LIN_USB_BREQ_DEVICE_CONFIG, 0, &cfg, sizeof(cfg));
}

bool LindeSharedDevice::getTimestamp(uint8_t channel, uint32_t &ts_ms)
{
    ts_ms = 0;
    return controlIn(LIN_USB_BREQ_TIMESTAMP, static_cast<uint16_t>(channel),
                     &ts_ms, sizeof(ts_ms));
}

bool LindeSharedDevice::setBusConfig(uint8_t channel, const lin_usb_bus_config_t &cfg)
{
    lin_usb_bus_config_t tmp = cfg;
    return controlOut(LIN_USB_BREQ_BAUDRATE, static_cast<uint16_t>(channel),
                      &tmp, sizeof(tmp));
}

bool LindeSharedDevice::setMode(uint8_t channel, uint8_t mode)
{
    lin_usb_mode_t m{};
    m.mode = mode;
    return controlOut(LIN_USB_BREQ_MODE, static_cast<uint16_t>(channel),
                      &m, sizeof(m));
}

bool LindeSharedDevice::uploadScheduleEntry(uint8_t channel, uint8_t table_id,
                                             uint8_t slot,
                                             const lin_usb_schedule_entry_t &entry)
{
    lin_usb_schedule_entry_t tmp = entry;
    tmp.table_id = table_id;
    /* wValue: slot index in high byte, channel in low byte */
    const uint16_t wValue = static_cast<uint16_t>((static_cast<uint16_t>(slot) << 8) | channel);
    return controlOut(LIN_USB_BREQ_SCHEDULE, wValue, &tmp, sizeof(tmp));
}

bool LindeSharedDevice::updateFrameConfig(uint8_t channel,
                                           const lin_usb_schedule_entry_t &entry)
{
    lin_usb_schedule_entry_t tmp = entry;
    return controlOut(LIN_USB_BREQ_FRAME_CONFIG, static_cast<uint16_t>(channel),
                      &tmp, sizeof(tmp));
}

bool LindeSharedDevice::scheduleStart(uint8_t channel, uint8_t table_id, uint8_t entry_count)
{
    lin_usb_mode_t m{};
    m.mode        = LIN_USB_MODE_START;
    m.table_id    = table_id;
    m.entry_count = entry_count;
    return controlOut(LIN_USB_BREQ_MODE, static_cast<uint16_t>(channel),
                      &m, sizeof(m));
}

bool LindeSharedDevice::scheduleStop(uint8_t channel)
{
    return setMode(channel, LIN_USB_MODE_STOP);
}

bool LindeSharedDevice::getBusState(uint8_t channel, lin_usb_bus_state_t &state)
{
    std::memset(&state, 0, sizeof(state));
    return controlIn(LIN_USB_BREQ_BUS_STATE, static_cast<uint16_t>(channel),
                     &state, sizeof(state));
}

bool LindeSharedDevice::goToSleep(uint8_t channel)
{
    lin_usb_sleep_wakeup_t cmd{};
    cmd.command = 0u; // sleep
    return controlOut(LIN_USB_BREQ_SLEEP_WAKEUP, static_cast<uint16_t>(channel),
                      &cmd, sizeof(cmd));
}

bool LindeSharedDevice::wakeup(uint8_t channel)
{
    lin_usb_sleep_wakeup_t cmd{};
    cmd.command = 1u; // wakeup
    return controlOut(LIN_USB_BREQ_SLEEP_WAKEUP, static_cast<uint16_t>(channel),
                      &cmd, sizeof(cmd));
}
