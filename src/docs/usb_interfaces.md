# USB adapter interfaces: gs_usb (CAN), lin_usb (LIN), aio_usb (I/O)

The STM32G473 adapter firmware (`Test2_G473VET`) enumerates as **one composite USB
device** with up to three vendor-specific interfaces. Each interface has its own
bulk endpoint pair and its own host driver in CANgaroo.

| Interface | Purpose | `bInterfaceProtocol` | Endpoints (IN / OUT) | Host driver (CANgaroo) |
|-----------|---------|----------------------|----------------------|------------------------|
| gs_usb  | CAN channels            | `0xFF` | `0x81` / `0x02` | SocketCAN (Linux, kernel `gs_usb`), `CandleApiDriver` (Windows) |
| lin_usb | LIN channels            | `0x01` | `0x83` / `0x04` | `LindeApiDriver` (libusb, all platforms) |
| aio_usb | Digital I/O + analog in | `0x02` | `0x85` / `0x06` | `AiodeDriver` (`AiodeApi`, GPIO Control window) |

## Common ground

**USB identity.** VID `0x1d50`, PID `0x606f`, the candleLight/gs_usb ID (the PID
is a placeholder, see the TODO in `usb_descriptors.c`). The ID alone does not
identify the adapter: host drivers must look for the interface with the right
class/subclass/protocol (`0xFF` / `0xFF` / protocol above). A plain candleLight
dongle has the same VID/PID but no LIN or AIO interface.

**Interface numbers** depend on which drivers are enabled in `usb_app_config.h`
(`GS_USB_ENABLED`, `LIN_USB_ENABLED`, `AIO_USB_ENABLED`): CAN is 0, LIN follows
CAN, AIO follows both. With all three enabled: CAN = 0, LIN = 1, AIO = 2.
Never hard-code them on the host; read the configuration descriptor.

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
SETUP stage. lin_usb also checks payload fields (table id, slot, DLC) in the DATA
stage and stalls there if they are invalid.

**Byte order.** All structures are packed and little-endian. Each interface has a
`HOST_FORMAT` request (bRequest 0) inherited from gs_usb; the firmware ignores its
value.

**Timestamps** are `HAL_GetTick()` milliseconds since firmware start, shared by all
interfaces and channels. Hosts read a `TIMESTAMP` value once when opening and
convert frame timestamps relative to it.

**Firmware layering.** Each `*_usb.c` file is only the USB transport. Bus access
lives behind weak hooks (`gs_engine_*`, `lin_engine_*`, `aio_hw_*`) that the
application overrides. Device-to-host traffic goes through a small ring queue
(`*_report_frame()` / `in_queue_push()`) and is pumped from `*_usb_task()` in the
main loop.

**Multiplexing.** gs_usb and lin_usb carry several channels over **one** bulk
endpoint pair; every frame has a `channel` field. A host driver must own a single
handle and reader per physical device and dispatch frames to per-channel queues
(see `CandleSharedDevice` and `LindeSharedDevice`). Opening one libusb handle per
channel does not work: only one handle can claim the interface.

---

## gs_usb (CAN)

Compatible with the Linux kernel `gs_usb` driver (`drivers/net/can/usb/gs_usb.c`) and
the candleLight Windows API. Firmware: `Core/Inc/gs_usb.h`, `Core/Src/gs_usb.c`,
config in `gs_usb_config.h` (`GS_USB_CAN_CHANNEL_COUNT`, currently 2; FDCAN clock
96 MHz).

### Control requests

| bRequest | Name | Dir | `wValue` | Payload | Notes |
|---------:|------|-----|----------|---------|-------|
| 0  | `HOST_FORMAT`     | OUT | –       | `gs_host_config_t` (4 B)   | value ignored |
| 1  | `BITTIMING`       | OUT | channel | `gs_device_bittiming_t` (20 B) | prop_seg, phase_seg1, phase_seg2, sjw, brp |
| 2  | `MODE`            | OUT | channel | `gs_device_mode_t` (8 B)   | `mode`: 0 reset, 1 start; `flags`: `GS_CAN_FLAG_*` |
| 3  | `BERR`            | IN  | –       | zeros                      | not implemented |
| 4  | `BT_CONST`        | IN  | channel | `gs_device_bt_const_t` (40 B) | features, clock, timing limits |
| 5  | `DEVICE_CONFIG`   | IN  | –       | `gs_device_config_t` (12 B) | `icount` = channels − 1, sw 2, hw 1 |
| 6  | `TIMESTAMP`       | IN  | –       | `uint32_t`                 | ms tick |
| 7  | `IDENTIFY`        | OUT | channel | up to 64 B                 | flashes LED |
| 8  | `GET_USER_ID`     | IN  | –       | zeros                      | not implemented |
| 11 | `SET_USER_ID`     | OUT | –       | discarded                  | not implemented |
| 12 | `GET_TERMINATION` | IN  | –       | zeros                      | not implemented |
| 13 | `SET_TERMINATION` | OUT | –       | discarded                  | not implemented |

Requests 9 (`DATA_BITTIMING`) and 10 (`BT_CONST_EXT`) are not handled (STALL), so
**CAN FD is not offered**. Advertised features: listen-only, loop-back, one-shot,
identify (no hardware timestamps, no triple sampling).

### Bulk frame: `gs_host_frame_t` (20 bytes, both directions)

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `echo_id`  | u32 | host TX: any id ≠ `0xFFFFFFFF`, echoed back; bus RX: `0xFFFFFFFF` |
| 4  | `can_id`   | u32 | SocketCAN layout: `0x80000000` EFF, `0x40000000` RTR, `0x20000000` ERR |
| 8  | `can_dlc`  | u8  | 0–8 |
| 9  | `channel`  | u8  | CAN channel index |
| 10 | `flags`    | u8  | |
| 11 | `reserved` | u8  | |
| 12 | `data[8]`  | u8×8 | payload |

- **OUT (host → device):** one frame to transmit. Frames with a wrong length or
  channel are dropped silently.
- **IN (device → host):** received frames and TX echoes, all channels mixed.
  A TX is confirmed when its echo comes back.

### Host side

- **Linux:** the kernel `gs_usb` driver binds interface 0 and creates `canX`
  netdevs; CANgaroo uses them through `SocketCanDriver`. Opening the LIN/AIO
  interfaces with libusb does not detach it.
- **Windows:** the BOS / MS OS 2.0 descriptor marks **interface 0 only** as
  WinUSB with the candleLight `DeviceInterfaceGUID`
  `{c15b4308-04d3-11e6-b3ea-6057189e6443}`, so `CandleApiDriver` finds it without
  a driver install.

---

## lin_usb (LIN)

Firmware: `Core/Inc/lin_usb.h`, `Core/Src/lin_usb.c`, config in `lin_usb_config.h`
(`LIN_USB_CHANNEL_COUNT` 2, `LIN_USB_MAX_SCHEDULE_TABLES` 4,
`LIN_USB_MAX_SCHEDULE_ENTRIES` 16). Host mirror: `src/driver/LindeApiDriver/lin_usb_protocol.h`
(keep in sync by hand).

The device **schedules frames itself**: the host uploads schedule tables, starts one,
and afterwards only updates publisher payloads and receives results.

### Control requests

| bRequest | Name | Dir | `wValue` | Payload | Notes |
|---------:|------|-----|----------|---------|-------|
| 0 | `HOST_FORMAT`   | OUT | channel (ignored) | `lin_usb_host_config_t` (4 B) | value ignored |
| 1 | `BAUDRATE`      | OUT | channel | `lin_usb_bus_config_t` (20 B) | baud, LIN version, break length, timebase, NAD, diag timings, `flags` (bit 0 = master) |
| 2 | `MODE`          | OUT | channel | `lin_usb_mode_t` (4 B) | `mode` 0 stop / 1 start / 2 pause; on start `table_id` + `entry_count` |
| 3 | `DEVICE_CONFIG` | IN  | –       | `lin_usb_device_config_t` (16 B) | tables per channel, baud bitmask, `icount` = channels − 1, versions, `LIN_USB_FEATURE_*` |
| 4 | `TIMESTAMP`     | IN  | –       | `uint32_t` | ms tick |
| 5 | `IDENTIFY`      | OUT | channel | none (`wLength` 0) | |
| 6 | `FRAME_CONFIG`  | OUT | channel | `lin_usb_schedule_entry_t` (16 B) | update an entry, matched by LIN ID |
| 7 | `SCHEDULE`      | OUT | `slot << 8 \| channel` | `lin_usb_schedule_entry_t` (16 B) | install entry into `table_id` / slot |
| 8 | `BUS_STATE`     | IN  | channel | `lin_usb_bus_state_t` (4 B) | 0 ok, 1 bus-off, 2 passive (stopped), 3 error |
| 9 | `SLEEP_WAKEUP`  | OUT | channel | `lin_usb_sleep_wakeup_t` (4 B) | `command` 0 sleep, 1 wakeup |

`SCHEDULE` and `FRAME_CONFIG` stall if `slot >= 16`, `table_id >= 4` or `dlc` is
outside 1–8. The slot limit is not reported by `DEVICE_CONFIG`; the host header
defines `LIN_USB_MAX_SCHEDULE_ENTRIES` to match.

Schedule entry (`lin_usb_schedule_entry_t`): `lin_id`, `direction` (0 = this node
publishes, 1 = subscribes), `dlc`, `flags` (`LIN_USB_FRAME_FLAG_*`, e.g. sporadic),
`period_ms`, `table_id`, `data[8]` (default payload for published frames).

### Bulk frame: `lin_usb_host_frame_t` (20 bytes, both directions)

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `echo_id`      | u32 | `0xFFFFFFFF` = received from bus; otherwise a published frame |
| 4  | `timestamp_ms` | u32 | device tick (IN only) |
| 8  | `lin_id`       | u8  | frame ID (0–63) |
| 9  | `channel`      | u8  | LIN channel index |
| 10 | `dlc`          | u8  | 1–8 |
| 11 | `flags`        | u8  | see below |
| 12 | `data[8]`      | u8×8 | payload |

Frame flags: `0x01` enhanced checksum, `0x02` subscriber, `0x04` error,
`0x08` TX update, `0x10` sporadic, `0x20` responded, `0x40` valid checksum,
`0x80` sleep. On an error frame, a missing `RESPONDED` means no slave answered
and a missing `VALID` means a checksum error.

- **OUT (host → device), "set frame":** sets the payload the matching publisher
  entry sends on its next slot. IDs not in the running schedule are ignored;
  frames with `dlc` outside 1–8 or a bad channel are dropped.
- **IN (device → host):** results of scheduled slots (published echoes and
  subscribed responses), all channels mixed.

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
interfaces named `Linde<device>_CH<channel>`. `LindeSharedDevice` owns the libusb
handle, one reader thread and the per-channel queues; each `LindeApiInterface`
reference-counts the shared device.

---

## aio_usb (digital I/O + analog)

Firmware: `Core/Inc/aio_usb.h`, `Core/Src/aio_usb.c`, config in `aio_usb_config.h`
(32 I/O lines, 16 analog channels, 16-bit resolution). Host mirror:
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
| 6 | `READ_STATUS`   | IN  | –    | `aio_usb_report_t` (44 B) | full snapshot |

`IO_CONFIG` stalls for `line >= 32`. An unknown `mode` is ignored without a stall.
Configuring an output applies `default_state` immediately.

### Report: `aio_usb_report_t` (44 bytes)

| Offset | Field | Type | Meaning |
|-------:|-------|------|---------|
| 0  | `timestamp_ms` | u32     | device tick at sampling |
| 4  | `io_states`    | u32     | level of line *i* in bit *i* |
| 8  | `io_direction` | u32     | bit *i*: 1 output, 0 input |
| 12 | `analog[16]`   | u16×16  | raw right-aligned ADC values |

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

1. Update the firmware header (`Core/Inc/*_usb.h`) and handler (`Core/Src/*_usb.c`):
   check `wValue` in the SETUP stage, check payload fields in the DATA stage, and
   return `false` to STALL on invalid input.
2. Mirror the change in the host protocol header (`lin_usb_protocol.h` /
   `aio_usb_protocol.h`); gs_usb must stay compatible with the kernel driver.
3. Keep structures packed and the sizes identical on both sides. A size mismatch
   shows up as a STALL (control transfers) or silently dropped frames (bulk).
