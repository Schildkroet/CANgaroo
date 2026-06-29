#pragma once

#include "lin_usb_protocol.h"

#include <libusb.h>

#include <QList>
#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// Shared state for all LIN channels on a single physical lin_usb device.
//
// The device multiplexes all channels through one USB bulk IN endpoint;
// each received lin_usb_host_frame_t carries a channel field. This struct
// owns exactly ONE libusb handle and ONE reader thread per physical device.
// All LindeApiInterface instances for the same device share it and read from
// their per-channel queue.
struct LindeSharedDevice
{
    static constexpr int MAX_CHANNELS = 4;

    libusb_context       *ctx{nullptr};
    libusb_device_handle *handle{nullptr};
    uint8_t               ep_in{0};
    uint8_t               ep_out{0};
    uint8_t               itf{0};
    uint8_t               channelCount{0};
    uint32_t              features{0};   // LIN_USB_FEATURE_* bitmask

    QString productName{"lindeapi"};
    int     deviceIndex{0};

    // Reference-counted open/close — protected by openMutex.
    QMutex openMutex;
    int    openCount{0};

    // Serialises bulk OUT transfers across channels (one shared endpoint).
    QMutex writeMutex;

    // Per-channel receive queues fed by the background reader thread.
    QMutex         queueMutex;
    QWaitCondition queueCond;
    QList<lin_usb_host_frame_t> rxQueues[MAX_CHANNELS];

    // Timestamp epoch captured at first open.
    QMutex  timestampMutex;
    uint32_t deviceTicksStart_ms{0};
    int64_t  hostOffsetStart_ms{0};
    bool     timestampValid{false};

    // Background reader thread.
    std::thread        readerThread;
    std::atomic<bool>  readerRunning{false};

    std::string lastError;

    ~LindeSharedDevice();

    // Scan for the first matching lin_usb device, open and claim the LIN
    // interface. Creates and owns its own libusb_context.
    // Called by LindeApiInterface::open() when openCount reaches 0→1.
    bool open();
    void close();

    void startReader();
    void stopReader();
    void resetTimestampEpoch();

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
    static constexpr uint8_t BRT_VENDOR_ITF_OUT = 0x41u;
    static constexpr uint8_t BRT_VENDOR_ITF_IN  = 0xC1u;
    static constexpr unsigned CTRL_TIMEOUT_MS   = 1000u;
    static constexpr unsigned BULK_TIMEOUT_MS   = 50u;

    bool controlOut(uint8_t breq, uint16_t wValue, void *data, uint16_t len);
    bool controlIn (uint8_t breq, uint16_t wValue, void *data, uint16_t len);
};
