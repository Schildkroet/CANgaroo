# USB adapter interfaces: gs_usb (CAN), lin_usb (LIN), aio_usb (I/O)

The STM32G473 adapter firmware enumerates as **one composite USB device** with up to three
vendor-specific interfaces. Each interface has its own bulk endpoint pair and its
own host driver in CANgaroo.

The device side is available as a reference implementation:
[`firmware/STM32G4_TinyUSB_CanLinAio/`](../../firmware/STM32G4_TinyUSB_CanLinAio/README.md),
a bare STM32CubeIDE project (STM32G473, TinyUSB 0.21) with all three class
drivers, weak no-op hooks where the bus code plugs in, and a standalone libusb
host sample (`SampleApp/`). Firmware paths below are relative to its `Core/`
directory. Channel counts, clocks and I/O sizes quoted below are that project's
defaults; a real adapter sizes them for its hardware and reports them in
`DEVICE_CONFIG` / `BT_CONST`, so hosts must always read them from the device.

| Interface | Purpose | `bInterfaceProtocol` | Endpoints (IN / OUT) | Host driver (CANgaroo) |
|-----------|---------|----------------------|----------------------|------------------------|
| gs_usb  | CAN channels            | `0xFF` | `0x81` / `0x02` | SocketCAN (Linux, kernel `gs_usb`), `CandleApiDriver` (Windows) |
| lin_usb | LIN channels            | `0x01` | `0x83` / `0x04` | `LindeApiDriver` (all platforms) |
| aio_usb | Digital I/O + analog in | `0x02` | `0x85` / `0x06` | `AiodeDriver` (`AiodeApi`, GPIO Control window) |

## Common ground

**USB identity.** VID `0x1d50`, PID `0x606f`, the candleLight/gs_usb ID — the
exact ID the Linux kernel `gs_usb` driver binds to, so it is deliberate and not a
placeholder (see the header comment in `usb_descriptors.c`). The ID alone does not
identify the adapter: host drivers must look for the interface with the right
class/subclass/protocol (`0xFF` / `0xFF` / protocol above). A plain candleLight
dongle has the same VID/PID but no LIN or AIO interface.

**Interface numbers** depend on which drivers are enabled in `usb_app_config.h`
(`GS_USB_ENABLED`, `LIN_USB_ENABLED`, `AIO_USB_ENABLED`): CAN is 0, LIN follows
CAN, AIO follows both. With all three enabled: CAN = 0, LIN = 1, AIO = 2.
Never hard-code them on the host; read the configuration descriptor. The Linux
kernel `gs_usb` driver binds interface 0 of 1d50:606f regardless of its class,
so firmware built without CAN must use a different VID/PID (the firmware build
warns).

**Control requests** are vendor requests to the interface:

| Direction | `bmRequestType` | `wIndex` | `wValue` |
|-----------|-----------------|----------|----------|
| host → device | `0x41` | interface number | request specific (usually the channel index, low byte) |
| device → host | `0xC1` | interface number | request specific |

Requests whose `wIndex` does not match an interface are ignored by that interface's
handler, so the same `bRequest` numbers can mean different things on each interface.
All firmware handlers share one dispatcher (`tud_vendor_control_xfer_cb` in
`usb_app_drivers.c`).

**Errors are STALLs.** An unknown request or an out-of-range channel/line index
stalls EP0; libusb reports `LIBUSB_ERROR_PIPE`. Checks on `wValue` happen in the
SETUP stage. lin_usb additionally validates payload fields (table id, slot, DLC)
in the DATA stage. TinyUSB arms the status stage only after the DATA-stage
callback returned `true`, so returning `false` there STALLs as well: an invalid
payload is rejected, never silently dropped. Firmware before this change ACKed
such requests and ignored them, so hosts should treat a STALL as "rejected" but
not rely on getting one from older adapters.

**Byte order.** All structures are packed and little-endian. Each interface has a
`HOST_FORMAT` request (bRequest 0) inherited from gs_usb; the firmware ignores its
value.

**Timestamps.** lin_usb and aio_usb use `HAL_GetTick()` milliseconds since
firmware start. gs_usb uses a free-running 32-bit **microsecond** counter (wraps
after ~71 min), both for `TIMESTAMP` and for the per-frame `timestamp_us`, as the
Linux driver expects. Hosts read a `TIMESTAMP` value once when opening and
convert frame timestamps relative to it.

**Firmware layering.** Each `*_usb.c` file is only the USB transport. Bus access
lives behind weak hooks (`gs_engine_*`, `lin_engine_*`, `aio_hw_*`) that the
application overrides. Device-to-host traffic goes through a small ring queue
(`*_report_frame()` / `in_queue_push()`). A frame is sent as soon as the IN
endpoint is free, otherwise from the transfer-complete callback or
`*_usb_task()`; nothing is sent before the device is configured, and a frame
leaves the queue only once its transfer has started.

**Multiplexing.** gs_usb and lin_usb carry several channels over **one** bulk
endpoint pair; every frame has a `channel` field. A host driver must own a single
handle and reader per physical device and dispatch frames to per-channel queues
(see `CandleSharedDevice` and `LindeSharedDevice`). Opening one libusb handle per
channel does not work: only one handle can claim the interface.

---

## gs_usb (CAN)

Compatible with the Linux kernel `gs_usb` driver (`drivers/net/can/usb/gs_usb.c`) and
the candleLight Windows API. Firmware: [`Src/gs_usb.c`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Src/gs_usb.c) /
[`Inc/gs_usb.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/gs_usb.h) (transport, weak `gs_engine_*` hooks for the
FDCAN code), config in [`Inc/gs_usb_config.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/gs_usb_config.h)
(`GS_USB_CAN_CHANNEL_COUNT` 2, FDCAN clock 96 MHz). The clock differs between
boards, so the host must use `fclk_can` from `BT_CONST`, not a constant.
Request numbers follow the kernel driver; the candle_api source comments
disagree on 9–13.

### Control requests

| bRequest | Name | Dir | `wValue` | Payload | Notes |
|---------:|------|-----|----------|---------|-------|
| 0  | `HOST_FORMAT`     | OUT | –       | `gs_host_config_t` (4 B)   | value ignored |
| 1  | `BITTIMING`       | OUT | channel | `gs_device_bittiming_t` (20 B) | prop_seg, phase_seg1, phase_seg2, sjw, brp |
| 2  | `MODE`            | OUT | channel | `gs_device_mode_t` (8 B)   | `mode`: 0 reset, 1 start; `flags`: `GS_CAN_FLAG_*` |
| 3  | `BERR`            | IN  | –       | zeros                      | not implemented |
| 4  | `BT_CONST`        | IN  | channel | `gs_device_bt_const_t` (40 B) | features, clock, timing limits |
| 5  | `DEVICE_CONFIG`   | IN  | –       | `gs_device_config_t` (12 B) | `icount` = channels − 1, sw 2, hw 1 |
| 6  | `TIMESTAMP`       | IN  | –       | `uint32_t`                 | µs counter |
| 7  | `IDENTIFY`        | OUT | channel | up to 64 B                 | flashes LED |
| 8  | `GET_USER_ID`     | IN  | –       | zeros                      | not implemented |
| 9  | `SET_USER_ID`     | OUT | –       | discarded                  | not implemented |
| 10 | `DATA_BITTIMING`  | OUT | channel | `gs_device_bittiming_t` (20 B) | CAN FD data phase, applied on the next `MODE` start with `GS_CAN_FLAG_FD` |
| 11 | `BT_CONST_EXT`    | IN  | channel | `gs_device_bt_const_ext_t` (72 B) | `BT_CONST` + data-phase limits (dtseg1/2, dsjw, dbrp) |
| 12 | `SET_TERMINATION` | OUT | –       | discarded                  | not implemented |
| 13 | `GET_TERMINATION` | IN  | –       | zeros                      | not implemented |
| 14 | `GET_STATE`       | IN  | channel | `gs_device_state_t` (12 B) | `state` (0 error-active … 3 bus-off, 4 stopped, 5 sleeping), `rxerr`, `txerr` |

Advertised features come from `GS_USB_FEATURES` in `gs_usb_config.h`, so each
adapter lists only what its CAN code implements. The default set: listen-only,
HW timestamp, identify, **FD**
(`0x100`), `BT_CONST_EXT` (`0x400`), `GET_STATE` (`0x2000`) and the private
`AUTO_RESTART` (bit 31: the channel recovers from bus-off by itself when started
with mode flag bit 31; Linux masks it out). Loop-back and one-shot are not in
the default set. Mode flags used: `LISTEN_ONLY 0x1`, `HW_TIMESTAMP 0x10`, `FD 0x100`,
`AUTO_RESTART 0x80000000`.

### Bulk frame: `gs_host_frame_t`

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `echo_id`  | u32 | host TX: any id ≠ `0xFFFFFFFF`, echoed back; bus RX: `0xFFFFFFFF` |
| 4  | `can_id`   | u32 | SocketCAN layout: `0x80000000` EFF, `0x40000000` RTR, `0x20000000` ERR |
| 8  | `can_dlc`  | u8  | classic: 0–8 bytes; FD: DLC code 0–15 |
| 9  | `channel`  | u8  | CAN channel index |
| 10 | `flags`    | u8  | `0x01` overflow, `0x02` FD, `0x04` BRS, `0x08` ESI |
| 11 | `reserved` | u8  | |
| 12 | `data[8]` / `data[64]` | u8 | payload (classic / FD) |
| 20 / 76 | `timestamp_us` | u32 | IN only, once the channel was started with `HW_TIMESTAMP` |

Frame sizes on the wire: classic 20 B, classic + timestamp 24 B, FD 76 B,
FD + timestamp 80 B. An FD frame spans two 64-byte bulk packets; every frame ends
with a short packet, so a host reading into an 80-byte buffer gets exactly one
frame per transfer.

- **OUT (host → device):** one frame to transmit, 20 B classic or 76 B FD
  (trailing padding / timestamp is ignored). A frame shorter than its payload or
  with a bad channel is dropped. When the device TX queue is full the frame is
  kept and the OUT endpoint NAKs until it fits, so frames are never lost.
- **IN (device → host):** received frames, TX echoes and error frames, all
  channels mixed. A TX is confirmed when its echo comes back, which happens only
  after the frame was ACKed on the bus. Error frames carry `GS_CAN_ERR_FLAG` and
  SocketCAN error classes (`linux/can/error.h`) in `can_id`. FD frames are only
  sent to a channel started with `GS_CAN_FLAG_FD`. When the host falls behind,
  bus RX frames are dropped first; the last queue slots are reserved for echoes
  and error frames.

### Host side

- **Linux:** the kernel `gs_usb` driver binds interface 0 and creates `canX`
  netdevs; CANgaroo uses them through `SocketCanDriver`. Opening the LIN/AIO
  interfaces with libusb does not detach it.
- **Windows:** the BOS / MS OS 2.0 descriptor marks every interface as WinUSB,
  each with its own `DeviceInterfaceGUID`, so no driver install is needed:

  | Interface | `DeviceInterfaceGUID` | Opened by |
  | :-- | :-- | :-- |
  | gs_usb  | `{c15b4308-04d3-11e6-b3ea-6057189e6443}` (candleLight) | `CandleApiDriver` |
  | lin_usb | `{dfaa1f65-e194-414c-ac5d-66ea6a8ba9c9}` | `LindeApiDriver` |
  | aio_usb | `{4c86c041-3321-446b-ba72-6a4be9f1c2b0}` | `AiodeDriver` |

  Each host driver opens only its own interface's device node. WinUSB allows one
  handle per interface, and `libusb_open()` opens *all* WinUSB interfaces of a
  composite device, so libusb would fail with "access denied" as soon as another
  driver holds a sibling interface. That is why lin_usb and aio_usb go through
  `UsbVendorInterface` (`src/driver/UsbVendorInterface/`): libusb on
  Linux/macOS, WinUSB by GUID on Windows. The GUIDs are duplicated in
  `LindeSharedDevice.h` and `AiodeApi.hpp`; keep them in sync with the firmware.

---

## lin_usb (LIN)

Firmware: [`Src/lin_usb.c`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Src/lin_usb.c) /
[`Inc/lin_usb.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/lin_usb.h) (USB transport, weak `lin_engine_*` hooks
for the LIN scheduler), config in [`Inc/lin_usb_config.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/lin_usb_config.h)
(`LIN_USB_CHANNEL_COUNT` 2, `LIN_USB_MAX_SCHEDULE_TABLES` 4,
`LIN_USB_MAX_SCHEDULE_ENTRIES` 16). Host mirror:
`src/driver/LindeApiDriver/lin_usb_protocol.h` (keep in sync by hand).

The device **schedules frames itself**: the host uploads schedule tables, starts one,
and afterwards only updates publisher payloads and receives results.

### Control requests

| bRequest | Name | Dir | `wValue` | Payload | Notes |
|---------:|------|-----|----------|---------|-------|
| 0 | `HOST_FORMAT`   | OUT | channel (ignored) | `lin_usb_host_config_t` (4 B) | value ignored |
| 1 | `BAUDRATE`      | OUT | channel | `lin_usb_bus_config_t` (20 B) | baud, LIN version, break length, timebase, NAD, diag timings, `flags` (bit 0 = master, bit 1 = listen-only) |
| 2 | `MODE`          | OUT | channel | `lin_usb_mode_t` (4 B) | `mode` 0 stop / 1 start / 2 pause; on start `table_id` + `entry_count` |
| 3 | `DEVICE_CONFIG` | IN  | –       | `lin_usb_device_config_t` (16 B) | tables per channel, slots per table (`schedule_entries`), baud bitmask, `icount` = channels − 1, `sw_version` (`major << 16 \| minor << 8 \| patch`), `hw_version` (plain number), `LIN_USB_FEATURE_*` |
| 4 | `TIMESTAMP`     | IN  | –       | `uint32_t` | ms tick |
| 5 | `IDENTIFY`      | OUT | channel | none (`wLength` 0) | |
| 6 | `FRAME_CONFIG`  | OUT | channel | `lin_usb_schedule_entry_t` (16 B) | update an entry, matched by LIN ID |
| 7 | `SCHEDULE`      | OUT | `slot << 8 \| channel` | `lin_usb_schedule_entry_t` (16 B) | install entry into `table_id` / slot |
| 8 | `BUS_STATE`     | IN  | channel | `lin_usb_bus_state_t` (4 B) | `state` 0 ok, 1 bus-off, 2 passive (deprecated), 3 error, 4 stopped, 5 sleeping; `dropped` (u16) = frames lost because the device IN queue was full |
| 9 | `SLEEP_WAKEUP`  | OUT | channel | `lin_usb_sleep_wakeup_t` (4 B) | `command` 0 sleep, 1 wakeup |

`SCHEDULE` and `FRAME_CONFIG` reject an entry with
`slot >= LIN_USB_MAX_SCHEDULE_ENTRIES`, `table_id >= LIN_USB_MAX_SCHEDULE_TABLES`
or a `dlc` outside 1–8, and `MODE` start rejects an out-of-range `table_id`,
all with a STALL (see *Errors are STALLs* above). An out-of-range `SCHEDULE`
slot is already rejected in the SETUP stage, before any data is sent. Both limits come from `DEVICE_CONFIG` (`schedule_tables`,
`schedule_entries`); the host constant `LIN_USB_MAX_SCHEDULE_ENTRIES` is only a
fallback for firmware that reports `schedule_entries = 0`.

Feature bits (`lin_usb_device_config_t.features`): `SCHEDULING 0x0001`,
`TIMESTAMP 0x0002`, `CUSTOM_BAUDRATE 0x0004`, `BUS_STATE 0x0008`,
`LISTEN_ONLY 0x0010`. A host that wants monitor mode must check `LISTEN_ONLY`
before setting the bus-config flag — older firmware treats it as a plain slave.

Schedule entry (`lin_usb_schedule_entry_t`): `lin_id`, `direction` (0 = this node
publishes, 1 = subscribes), `dlc`, `flags` (`LIN_USB_FRAME_FLAG_*`, e.g. sporadic),
`period_ms`, `table_id`, `data[8]` (default payload for published frames).

### Bulk frame: `lin_usb_host_frame_t` (20 bytes, both directions)

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `echo_id`      | u32 | `0xFFFFFFFF` = received from bus; `0` = ack of a bulk-OUT set-frame (no bus event); otherwise a published frame |
| 4  | `timestamp_ms` | u32 | device tick (IN only) |
| 8  | `lin_id`       | u8  | OUT: frame ID (0–63). IN: protected ID as seen on the bus (ID + parity bits 6/7); mask with `0x3F` for the frame ID |
| 9  | `channel`      | u8  | LIN channel index |
| 10 | `dlc`          | u8  | 1–8 |
| 11 | `flags`        | u8  | see below |
| 12 | `data[8]`      | u8×8 | payload |

Frame flags: `0x01` enhanced checksum, `0x02` subscriber, `0x04` error,
`0x08` wakeup (device → host; `LIN_USB_FRAME_FLAG_TX_UPDATE` is the historic name
for the same bit), `0x10` sporadic, `0x20` responded, `0x40` valid checksum,
`0x80` sleep. On an error frame, a missing `RESPONDED` means no slave answered
and a missing `VALID` means a checksum error.

- **OUT (host → device), "set frame":** sets the payload the matching publisher
  entry sends on its next slot. The device answers **every** OUT packet with an
  IN frame carrying `echo_id = 0` and the packet echoed back. If the IN queue is
  full, the ACK is held and the OUT endpoint NAKs the next packet until it fits,
  so ACKs are never dropped (bus frames can be; see `dropped` in `BUS_STATE`). `FLAG_ERROR` set
  means the update was not applied: the LIN ID is not a publisher in the running
  schedule, or the packet was invalid (`dlc` outside 1–8, bad channel, wrong
  length; a short packet is acked with an all-zero frame).
- **IN (device → host):** results of scheduled slots (published echoes and
  subscribed responses), sleep/wakeup events and set-frame acks, all channels
  mixed.

### Typical open sequence (per channel)

1. `DEVICE_CONFIG` once per device → channel count, tables, features.
2. `HOST_FORMAT`, `BAUDRATE` (role and timing), `MODE` stop.
3. Master: `SCHEDULE` for each table/slot from the LDF, then `MODE` start with
   the selected table. Slave: the frames this node publishes go into table 0,
   then `MODE` start (also with 0 entries, so the slave keeps listening).
4. Runtime: bulk OUT to change payloads, `MODE` start with another table to
   switch schedules, `BUS_STATE` for status, `MODE` stop on close.

### Host side

`LindeApiDriver` enumerates every device with a LIN interface and creates
interfaces named `Linde<device>_CH<channel>`. `LindeSharedDevice` owns the USB
handle (`UsbVendorInterface`), one reader thread and the per-channel queues; each `LindeApiInterface`
reference-counts the shared device.

---

## aio_usb (digital I/O + analog)

Firmware: [`Src/aio_usb.c`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Src/aio_usb.c) /
[`Inc/aio_usb.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/aio_usb.h), config in
[`Inc/aio_usb_config.h`](../../firmware/STM32G4_TinyUSB_CanLinAio/Core/Inc/aio_usb_config.h) (32 I/O lines, 16 analog
channels, 16 bit; a board with two lines and a 12-bit ADC sets 2 / 2 / 12). The
host must use the counts from `DEVICE_CONFIG`. Host mirror:
`src/driver/AiodeDriver/aio_usb_protocol.h`. There are no channels; `wValue`
selects an I/O line where needed.

### Control requests

| bRequest | Name | Dir | `wValue` | Payload | Notes |
|---------:|------|-----|----------|---------|-------|
| 0 | `HOST_FORMAT`   | OUT | –    | `aio_usb_host_config_t` (4 B) | value ignored |
| 1 | `DEVICE_CONFIG` | IN  | –    | `aio_usb_caps_t` (24 B) | line/analog counts, resolution, input/output capability masks, versions, `AIO_USB_FEATURE_*` |
| 2 | `TIMESTAMP`     | IN  | –    | `uint32_t` | ms tick |
| 3 | `IDENTIFY`      | OUT | –    | none (`wLength` 0) | |
| 4 | `IO_CONFIG`     | OUT | line | `aio_usb_io_config_t` (8 B) | `mode` 0 in / 1 out, `default_state`, `flags` (pull-up `0x01`, pull-down `0x02`, active-low `0x04`), `auto_report_ms` |
| 5 | `IO_SET`        | OUT | –    | `aio_usb_io_set_t` (8 B) | `mask` of lines to write, `values` |
| 6 | `READ_STATUS`   | IN  | –    | `aio_usb_report_t` (12 + 2 × analog count B) | full snapshot |

`IO_CONFIG` stalls for `line >= io_count` (from `DEVICE_CONFIG`; 32 by default). An unknown `mode` is ignored without a stall.
Configuring an output applies `default_state` immediately.

### Report: `aio_usb_report_t` (12 + 2 × analog count bytes)

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `timestamp_ms` | u32     | device tick at sampling |
| 4  | `io_states`    | u32     | level of line *i* in bit *i* |
| 8  | `io_direction` | u32     | bit *i*: 1 output, 0 input |
| 12 | `analog[n]`    | u16×n   | raw right-aligned ADC values, `n` = firmware `AIO_USB_ANALOG_COUNT` (44 B total with the default 16 channels, 16 B with 2) |

### Bulk endpoints

- **OUT:** an 8-byte `aio_usb_io_set_t`, same effect as `IO_SET`, without the
  control-transfer overhead.
- **IN:** auto-report snapshots. Every line with `auto_report_ms != 0` has its own
  timer; when any timer is due, **one** full report is sent and all due timers
  advance, so lines with the same period share a report.

### Host side

`AiodeApi` implements `GpioProvider` for the GPIO Control window
(`AiodeApi::scan()` opens every AIO device). It polls `READ_STATUS` from a
background thread and does not use the bulk IN auto-report. The `GpioProvider`
interface uses 16-bit direction/output masks, so only lines 0–15 can be driven
from CANgaroo, although the protocol addresses 32.

---

## Adding or changing a request

1. Update the firmware header (`Core/Inc/*_usb.h`) and handler
   (`Core/Src/*_usb.c`) of the reference firmware: check `wValue` in the SETUP
   stage and return `false` to STALL on an invalid index; check payload fields
   in the DATA stage and return `false` there too, which STALLs the status
   stage. Update its SampleApp as well.
2. Mirror the change in the host protocol header (`lin_usb_protocol.h` /
   `aio_usb_protocol.h`); gs_usb must stay compatible with the kernel driver.
3. Keep structures packed and the sizes identical on both sides. A size mismatch
   shows up as a STALL (control transfers) or silently dropped frames (bulk).
