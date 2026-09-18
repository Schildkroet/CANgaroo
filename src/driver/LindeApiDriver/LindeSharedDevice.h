#pragma once

#include "lin_usb_protocol.h"
#include "driver/UsbVendorInterface/UsbVendorInterface.h"

#include <QList>
#include <QMutex>
#include <QReadWriteLock>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// Shared state for all LIN channels on a single physical lin_usb device.
//
// The device multiplexes all channels through one USB bulk IN endpoint;
// each received lin_usb_host_frame_t carries a channel field. This struct
// owns exactly ONE USB handle and ONE reader thread per physical device.
// All LindeApiInterface instances for the same device share it and read from
// their per-channel queue.
struct LindeSharedDevice
{
    static constexpr int MAX_CHANNELS = 4;

    // Frames buffered per channel before the oldest are dropped (counted as overruns).
    static constexpr qsizetype MAX_QUEUED_FRAMES = 4096;

    // DeviceInterfaceGUID of the LIN interface in the firmware's MS OS 2.0 descriptor.
    static constexpr UsbVendorInterface::Id USB_ID{
        .vid = LIN_USB_VID,
        .pid = LIN_USB_PID,
        .protocol = LIN_USB_ITF_PROTOCOL,
        .windowsGuid = "{dfaa1f65-e194-414c-ac5d-66ea6a8ba9c9}",
    };

    UsbVendorInterface    usb;
    uint8_t               channelCount{0};   // clamped to MAX_CHANNELS
    uint8_t               scheduleTables{0};  // per channel, as reported by the device
    uint8_t               scheduleEntries{0}; // slots per table, as reported by the device
    uint32_t              features{0};       // LIN_USB_FEATURE_* bitmask
    uint32_t              swVersion{0};      // firmware version from DEVICE_CONFIG, major << 16 | minor << 8 | patch
    uint32_t              hwVersion{0};      // hardware revision from DEVICE_CONFIG

    QString productName{"Linde"};
    int     deviceIndex{0};   // n-th lin_usb device exposing the LIN interface

    // Reference-counted open/close. Held across the whole open()/startReader()
    // and stopReader()/close() sequences so channels cannot interleave them.
    QMutex openMutex;
    int    openCount{0};

    // Transfers hold this for reading, close() for writing, so a GUI-thread
    // query cannot use the handle while another thread tears the device down.
    QReadWriteLock handleLock;

    // Serialises bulk OUT transfers across channels (one shared endpoint).
    QMutex writeMutex;

    // Serialises EP0 control transfers across channels/threads (open-time
    // configuration bursts plus runtime queries from the GUI thread).
    QMutex controlMutex;

    // Per-channel receive queues fed by the background reader thread.
    // channelOpen and rxOverruns are guarded by queueMutex as well.
    QMutex         queueMutex;
    QWaitCondition queueCond;
    QList<lin_usb_host_frame_t> rxQueues[MAX_CHANNELS];
    bool     channelOpen[MAX_CHANNELS]{};
    uint64_t rxOverruns[MAX_CHANNELS]{};

    // Timestamp epoch captured at first open.
    QMutex  timestampMutex;
    uint32_t deviceTicksStart_ms{0};
    int64_t  hostOffsetStart_ms{0};
    bool     timestampValid{false};

    // Background reader thread.
    std::thread        readerThread;
    std::atomic<bool>  readerRunning{false};

    // lastError may be written from any BusListener thread or the GUI thread and
    // read from another thread, so it is guarded by errorMutex. Use the
    // setLastError()/getLastError() helpers rather than touching it directly.
    mutable QMutex errorMutex;
    std::string    lastError;

    void        setLastError(const std::string &err);
    std::string getLastError() const;

    ~LindeSharedDevice();

    // Number of attached lin_usb devices that expose the LIN interface.
    static int enumerateDevices();

    // Open the deviceIndex-th lin_usb device and claim the LIN interface.
    // Called by LindeApiInterface::open() when openCount reaches 0→1.
    bool open();
    void close();

    void startReader();
    void stopReader();
    void resetTimestampEpoch();

    // Accept (or drop) received frames for a channel; always clears its queue.
    void     setChannelOpen(uint8_t channel, bool open);
    uint64_t getRxOverruns(uint8_t channel);

    // Convert device millisecond timestamp to host millisecond timestamp.
    int64_t deviceTimestampToHostMs(uint32_t ts_ms);

    // Block until a frame for channel arrives or timeout_ms elapses.
    bool readFrame(uint8_t channel, lin_usb_host_frame_t &frame, unsigned timeout_ms);

    // Send a frame (acquires writeMutex).
    bool sendFrame(const lin_usb_host_frame_t &frame);

    // ---- Per-channel protocol helpers ----
    bool setHostFormat(uint8_t channel);
    bool getDeviceConfig(lin_usb_device_config_t &cfg);
    bool getTimestamp(uint8_t channel, uint32_t &ts_ms);
    bool setBusConfig(uint8_t channel, const lin_usb_bus_config_t &cfg);
    bool setMode(uint8_t channel, uint8_t mode);
    bool uploadScheduleEntry(uint8_t channel, uint8_t table_id, uint8_t slot,
                             const lin_usb_schedule_entry_t &entry);
    bool updateFrameConfig(uint8_t channel, const lin_usb_schedule_entry_t &entry);
    bool scheduleStart(uint8_t channel, uint8_t table_id, uint8_t entry_count);
    bool scheduleStop(uint8_t channel);
    bool getBusState(uint8_t channel, lin_usb_bus_state_t &state);
    bool goToSleep(uint8_t channel);
    bool wakeup(uint8_t channel);

private:
    static constexpr unsigned CTRL_TIMEOUT_MS   = 1000u;
    static constexpr unsigned BULK_TIMEOUT_MS   = 50u;

    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
};
