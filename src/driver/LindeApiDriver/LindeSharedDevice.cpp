#include "LindeSharedDevice.h"

#include "core/Log.h"

#include <QMutexLocker>
#include <QDeadlineTimer>
#include <QDateTime>

#include <algorithm>
#include <chrono>
#include <cstring>

LindeSharedDevice::~LindeSharedDevice()
{
    stopReader();
    close(); // also frees ctx
}

bool LindeSharedDevice::findLinInterface(libusb_device *dev, uint8_t &itfNum,
                                         uint8_t &epIn, uint8_t &epOut)
{
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != 0)
        return false;
    if (desc.idVendor != LIN_USB_VID || desc.idProduct != LIN_USB_PID)
        return false;

    libusb_config_descriptor *cfg_desc = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg_desc) != 0)
        return false;

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
                itfNum = alt.bInterfaceNumber;
                for (uint8_t e = 0; e < alt.bNumEndpoints; e++)
                {
                    const libusb_endpoint_descriptor &ep = alt.endpoint[e];
                    if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                        epIn = ep.bEndpointAddress;
                    else
                        epOut = ep.bEndpointAddress;
                }
                foundItf = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg_desc);
    return foundItf;
}

int LindeSharedDevice::enumerateDevices()
{
    libusb_context *enumCtx = nullptr;
    if (libusb_init(&enumCtx) != 0)
        return 0;

    libusb_device **list = nullptr;
    const ssize_t cnt = libusb_get_device_list(enumCtx, &list);
    int count = 0;
    for (ssize_t i = 0; i < cnt; i++)
    {
        uint8_t itfNum = 0, epIn = 0, epOut = 0;
        if (findLinInterface(list[i], itfNum, epIn, epOut))
            count++;
    }
    if (cnt >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(enumCtx);
    return count;
}

bool LindeSharedDevice::open()
{
    // Always create a fresh context so re-open after close works cleanly.
    if (ctx)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }
    if (libusb_init(&ctx) != 0)
    {
        ctx = nullptr;
        setLastError("libusb_init failed");
        return false;
    }

    libusb_device **list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0)
    {
        setLastError("libusb_get_device_list failed");
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    // 1d50:606f is also the gs_usb (candleLight) ID, so only devices that
    // actually expose the LIN interface count towards deviceIndex.
    libusb_device *found = nullptr;
    int linIndex = 0;
    for (ssize_t i = 0; i < cnt && !found; i++)
    {
        if (!findLinInterface(list[i], itf, ep_in, ep_out))
            continue;
        if (linIndex++ == deviceIndex)
            found = list[i];
    }

    if (!found)
    {
        setLastError("no lin_usb device with a LIN interface at index " + std::to_string(deviceIndex));
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    int rc = libusb_open(found, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0)
    {
        if (rc == LIBUSB_ERROR_ACCESS)
            setLastError("permission denied (VID 0x1d50 / PID 0x606f). Add a udev rule or run as root.");
        else
            setLastError(libusb_strerror(static_cast<libusb_error>(rc)));
        handle = nullptr;
        libusb_exit(ctx);
        ctx = nullptr;
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
        kernelDriverDetached = true;
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

    // Computed as unsigned so icount 0xFF cannot wrap to zero channels.
    const unsigned reportedChannels = dcfg.icount + 1u;
    if (reportedChannels > MAX_CHANNELS)
    {
        log_warning(QStringLiteral("LindeAPI: device reports %1 channels, only the first %2 are supported")
                        .arg(reportedChannels).arg(MAX_CHANNELS));
    }
    channelCount   = static_cast<uint8_t>(std::min<unsigned>(reportedChannels, MAX_CHANNELS));
    scheduleTables = dcfg.schedule_tables;
    // Older firmware leaves the field zero; fall back to the historic value.
    scheduleEntries = dcfg.schedule_entries ? dcfg.schedule_entries
                                            : static_cast<uint8_t>(LIN_USB_MAX_SCHEDULE_ENTRIES);
    features       = dcfg.features;

    {
        QMutexLocker lock(&queueMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++)
        {
            channelOpen[ch] = false;
            rxOverruns[ch]  = 0;
            rxQueues[ch].clear();
        }
    }

    setLastError(std::string());
    return true;
}

void LindeSharedDevice::close()
{
    QWriteLocker lock(&handleLock);
    if (handle)
    {
        libusb_release_interface(handle, itf);
#ifndef _WIN32
        if (kernelDriverDetached)
            libusb_attach_kernel_driver(handle, itf);
#endif
        libusb_close(handle);
        handle               = nullptr;
        ep_in                = 0;
        ep_out               = 0;
        itf                  = 0;
        channelCount         = 0;
        scheduleTables       = 0;
        scheduleEntries      = 0;
        features             = 0;
        kernelDriverDetached = false;
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
        bool errorLogged = false;
        while (readerRunning.load(std::memory_order_relaxed))
        {
            lin_usb_host_frame_t frame{};
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle, ep_in,
                                                reinterpret_cast<unsigned char *>(&frame),
                                                static_cast<int>(sizeof(frame)),
                                                &transferred,
                                                BULK_TIMEOUT_MS);
            if (rc == LIBUSB_ERROR_TIMEOUT)
                continue;
            if (rc != 0)
            {
                // Errors like NO_DEVICE (unplugged) return immediately: log once
                // and back off instead of spinning a core.
                if (!errorLogged)
                {
                    log_error(QStringLiteral("LindeAPI: USB read failed: %1")
                                  .arg(QString::fromUtf8(libusb_strerror(static_cast<libusb_error>(rc)))));
                    errorLogged = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(BULK_TIMEOUT_MS));
                continue;
            }
            errorLogged = false;
            if (transferred != static_cast<int>(sizeof(frame)))
                continue;

            // channelCount only changes in open()/close(), while the reader is stopped.
            const uint8_t ch = frame.channel;
            if (ch >= channelCount)
            {
                log_error(QStringLiteral("LindeAPI: received frame for invalid channel %1")
                              .arg(static_cast<int>(ch)));
                continue;
            }

            QMutexLocker lock(&queueMutex);
            // Frames for a channel nobody has open would pile up and be
            // delivered stale on its next open.
            if (!channelOpen[ch])
                continue;
            auto &queue = rxQueues[ch];
            if (queue.size() >= MAX_QUEUED_FRAMES)
            {
                queue.removeFirst();
                rxOverruns[ch]++;
            }
            queue.append(frame);
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

void LindeSharedDevice::setChannelOpen(uint8_t channel, bool open)
{
    if (channel >= MAX_CHANNELS)
        return;
    QMutexLocker lock(&queueMutex);
    channelOpen[channel] = open;
    rxQueues[channel].clear();
    if (open)
        rxOverruns[channel] = 0;
    queueCond.wakeAll();
}

uint64_t LindeSharedDevice::getRxOverruns(uint8_t channel)
{
    if (channel >= MAX_CHANNELS)
        return 0;
    QMutexLocker lock(&queueMutex);
    return rxOverruns[channel];
}

void LindeSharedDevice::resetTimestampEpoch()
{
    uint32_t ts_ms = 0;
    // The device clock is shared by all channels; channel 0 always exists.
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
    if (channel >= MAX_CHANNELS)
        return false;

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
    QReadLocker handleGuard(&handleLock);
    if (!handle)
    {
        setLastError("device not open");
        return false;
    }

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
    QReadLocker handleGuard(&handleLock);
    if (!handle)
    {
        setLastError("device not open");
        return false;
    }

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
    QReadLocker handleGuard(&handleLock);
    if (!handle)
    {
        setLastError("device not open");
        return false;
    }

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
