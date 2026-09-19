# STM32G4 USB CAN / LIN / AIO adapter — reference project

A bare STM32CubeIDE project for the **STM32G473VET** that shows how to expose
three vendor-specific USB interfaces from one device with TinyUSB custom class
drivers:

| Interface | Driver | `bInterfaceProtocol` | Endpoints | Purpose |
| --- | --- | --- | --- | --- |
| CAN | `gs_usb` | `0xFF` | EP1 IN / EP2 OUT | [gs_usb](https://github.com/torvalds/linux/blob/master/drivers/net/can/usb/gs_usb.c) CAN / CAN FD protocol (candleLight compatible) |
| LIN | `lin_usb` | `0x01` | EP3 IN / EP4 OUT | Self-scheduling LIN master/slave/monitor protocol |
| AIO | `aio_usb` | `0x02` | EP5 IN / EP6 OUT | Generic digital I/O + analog inputs |

This project contains **only the USB transport**. The drivers relay every host
request to a set of weak hook functions (`gs_engine_*`, `lin_engine_*`,
`aio_hw_*`) whose default implementations are no-op stubs. The device
enumerates and answers every request, but it does not touch FDCAN, UART or GPIO.
To build a real adapter, you override those hooks with your own CAN, LIN and I/O
code (see [Integrating your engine](#integrating-your-engine)).

The same drivers run unchanged in a production adapter (2× CAN FD, 2× LIN),
where the hooks are implemented on top of an FDCAN driver and a LIN scheduler.

## Host compatibility

- **Linux:** the device uses VID `0x1d50` / PID `0x606f` (OpenMoko /
  candleLight). The in-kernel `gs_usb` driver binds to the CAN interface with no
  udev rules or modprobe configuration, including CAN FD. It works with
  `can-utils` and `python-can`.
- **Windows:** a BOS + MS OS 2.0 descriptor set assigns WinUSB to *every*
  interface, each with its own `DeviceInterfaceGUID`, so no INF file or Zadig
  step is needed:

  | Interface | DeviceInterfaceGUID |
  | --- | --- |
  | CAN | `{c15b4308-04d3-11e6-b3ea-6057189e6443}` (standard candleLight GUID, found by `candle_api`) |
  | LIN | `{dfaa1f65-e194-414c-ac5d-66ea6a8ba9c9}` |
  | AIO | `{4c86c041-3321-446b-ba72-6a4be9f1c2b0}` |

  The LIN and AIO GUIDs must differ from the CAN GUID. Otherwise `candle_api`
  also tries to open them as CAN channels. Windows caches this descriptor set
  per VID/PID/`bcdDevice`, so bump `bcdDevice` in `usb_descriptors.c` whenever
  you change it.
- **[CANgaroo](https://github.com/Schildkroet/CANgaroo)** supports all three
  interfaces out of the box: CAN via SocketCAN or `CandleApiDriver`, LIN via
  `LindeApiDriver`, and I/O in the GPIO Control window. Its
  `src/docs/usb_interfaces.md` is the detailed protocol reference.
- **SampleApp/** contains a small C++ host program (libusb) that drives all three
  interfaces (see [Sample host application](#sample-host-application)).

## Hardware and dependencies

- STM32G473VET, HSE 8 MHz → PLL ×24 ÷2 = 96 MHz SYSCLK and PCLK1 (FDCAN kernel clock).
- USB device FS clocked from HSI48, trimmed by the CRS against the host's
  start-of-frame packets. The CRS is enabled in `USER CODE BEGIN SysInit` in
  `main.c`: the LL clock code CubeMX generates configures the CRS but never
  clocks or enables it, so HSI48 would stay untrimmed and outside USB tolerance.
- [TinyUSB](https://github.com/hathach/tinyusb) **0.21.0**, vendored in
  `Core/TinyUSB/`: only `src/` (core, device stack, class drivers and the
  `st/stm32_fsdev` port). The drivers
  pass `is_isr` to `usbd_edpt_xfer()` (new in 0.21) according to the calling
  context, since frames may be reported from interrupts.
- STM32CubeG4 HAL/LL.

### Getting a buildable project

`Drivers/` (ST HAL/CMSIS) is not checked in; TinyUSB is:

1. Open `STM32G4_TinyUSB_CanLinAio.ioc` in STM32CubeIDE or CubeMX and run
   **Generate Code** to recreate `Drivers/`. Generation also rewrites the parts
   of `Core/` outside the `USER CODE` sections. Some of the USB integration
   lives there (the `#if GS_USB_ENABLED` / `LIN_USB_ENABLED` blocks in
   `main.c`), so restore those files afterwards, e.g.
   `git checkout -- Core` (`git checkout -- firmware/STM32G4_TinyUSB_CanLinAio/Core`
   inside CANgaroo).
2. **File → Import → Existing Projects into Workspace**, then build.

## File layout

| File | Role |
| --- | --- |
| `Core/Inc/usb_app_config.h` | `GS_USB_ENABLED` / `LIN_USB_ENABLED` / `AIO_USB_ENABLED`: select which interfaces are compiled in. Linux `gs_usb` binds interface 0 of 1d50:606f, so a build without CAN needs its own VID/PID (the build warns) |
| `Core/Src/usb_app_drivers.c` | Registers the enabled class drivers via `usbd_app_driver_get_cb()`. Dispatches vendor control requests by interface and answers the MS OS 2.0 request |
| `Core/Src/usb_descriptors.c` | Device, configuration, BOS/MS OS 2.0 and string descriptors. Interface numbers follow automatically from the enabled drivers. Serial number comes from the MCU UID |
| `Core/Inc/tusb_config.h` | TinyUSB config. Every built-in `CFG_TUD_*` class is 0 |
| `Core/TinyUSB/` | TinyUSB 0.21.0 (`src/` only) |
| `Core/{Inc,Src}/gs_usb*` | CAN interface: protocol, transport, weak `gs_engine_*` hooks |
| `Core/{Inc,Src}/lin_usb*` | LIN interface: protocol, transport, weak `lin_engine_*` hooks |
| `Core/{Inc,Src}/aio_usb*` | AIO interface: protocol, transport, weak `aio_hw_*` hooks |

`*_config.h` holds all compile-time settings for each driver: channel counts,
endpoints, the capabilities reported to the host, and version numbers.

## Usage

```c
tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
tusb_init(0, &dev_init);

gs_usb_init();
lin_usb_init();
aio_usb_init();

while (1)
{
    tud_task();
    gs_usb_task();
    lin_usb_task();
    aio_usb_task();
}
```

The `USB_LP` interrupt must call `tud_int_handler(0)` (see `stm32g4xx_it.c`).

## Integrating your engine

Each hook is declared in the driver's header and defined `__attribute__((weak))`
at the bottom of the driver's `.c` file. Provide a non-weak function with the same
signature in your own source file to replace it.

> Put the override in a source file that is linked **directly** into the
> executable, not in a static library. The linker only pulls an archive member
> in to resolve an *undefined* symbol, and the weak default already defines the
> hook. An override that lives only in a `.a` is silently ignored.

`gs_usb_report_frame()` and `lin_usb_report_frame()` can be called from
interrupt context. Queue access is protected by PRIMASK critical sections, and
nothing is sent before the host has configured the device (frames reported
earlier wait in the queue). They return `false` when the queue is full.
aio_usb needs no report call: it polls its `aio_hw_*` hooks from
`aio_usb_task()`.

### gs_usb (CAN / CAN FD)

| Hook | Called for |
| --- | --- |
| `gs_engine_set_bittiming(ch, bt)` | `BREQ_BITTIMING`: nominal bit timing. Store it; do not start the controller |
| `gs_engine_set_data_bittiming(ch, bt)` | `BREQ_DATA_BITTIMING`: CAN FD data phase |
| `gs_engine_set_mode(ch, mode)` | `BREQ_MODE`: start/stop. `mode->flags` carries `GS_CAN_FLAG_*` (`LISTEN_ONLY`, `FD`, `HW_TIMESTAMP`, `AUTO_RESTART`, …) |
| `bool gs_engine_send(ch, frame)` | Bulk OUT frame to transmit. Return `false` if the TX queue is full: the transport keeps the frame, NAKs the host and retries from `gs_usb_task()` |
| `gs_engine_get_state(ch, state)` | `BREQ_GET_STATE`: `GS_CAN_STATE_*` and the TX/RX error counters |
| `gs_engine_identify(ch)` | `BREQ_IDENTIFY`: blink an LED |
| `bool gs_engine_set_termination(ch, on)` | `BREQ_SET_TERMINATION`: switch the 120 Ω bus termination. Default returns `false` (request stalled) |
| `bool gs_engine_bus_off_recovery(ch)` | `BREQ_BUS_OFF_RECOVERY`: restart a bus-off channel the host started with `GS_CAN_FLAG_BUS_OFF_RECOVERY`. Default returns `false` (request stalled) |
| `gs_engine_suspend()` / `gs_engine_resume()` | USB suspend / resume (from `tud_suspend_cb()` / `tud_resume_cb()` in `usb_app_drivers.c`): take started channels off the bus, restore them. Default: no-op |
| `bool gs_engine_get_termination(ch, &on)` | `BREQ_GET_TERMINATION`. Default returns `false`. `GS_CAN_FEATURE_TERMINATION` is advertised for a channel only while this returns `true` |
| `gs_engine_can_clock_hz()` | FDCAN kernel clock reported in `BREQ_BT_CONST`. Default: `GS_USB_FDCAN_CLK_HZ` |
| `gs_engine_timestamp_us()` | 32-bit µs time base for `BREQ_TIMESTAMP` and frame timestamps. Default: `HAL_GetTick()` + SysTick |
| `gs_engine_task()` | Called from `gs_usb_task()`. Poll the controller here if you do not use RX interrupts |
| `gs_engine_reset()` | USB bus reset or unplug: stop every channel |

Report every received frame, TX echo and error frame with
`gs_usb_report_frame()`:

- **RX frames:** `echo_id = GS_ECHO_ID_RX`.
- **TX echoes:** use the host's `echo_id`. Send the echo only once the frame has
  actually been sent on the bus. The Linux driver stalls its TX path until the
  echo arrives.
- **Error frames:** set `GS_CAN_ERR_FLAG` in `can_id`, using the SocketCAN error
  classes defined in `gs_usb.h`.
- **CAN FD frames:** set `GS_FRAME_FLAG_FD` (and `BRS`/`ESI`) and store the DLC
  *code* 0–15 in `can_dlc`. Do this only while the host has started the channel
  with `GS_CAN_FLAG_FD`.

`gs_usb_report_frame()` stamps `timestamp_us` with the current time.
For bus frames and TX echoes, use `gs_usb_report_frame_at(frame, timestamp_us)`
instead, with the time taken in the controller interrupt (end of frame), so
main-loop latency does not end up in the timestamps. Use the same clock as
`gs_engine_timestamp_us()`. When the host falls behind, bus RX
frames are dropped first. The last `IN_QUEUE_RESERVED` queue slots are kept for
TX echoes and error frames. A frame that does not fit marks its channel as
overflowed. The channel's next frame then carries `GS_FRAME_FLAG_OVERFLOW`,
which Linux counts as an RX overrun. Call `gs_usb_report_overflow(ch)` when
frames are lost before they reach the transport (controller FIFO, engine
queue).

The feature bits advertised to the host come from `GS_USB_FEATURES` in
`gs_usb_config.h`. List only what your engine implements. Engine rules for the
error-handling features:

- **`BERR_REPORTING`:** send bus-error frames (`CAN_ERR_PROT`/`CAN_ERR_BUSERROR`)
  only while the host set `GS_CAN_FLAG_BERR_REPORTING`. Always report state
  changes and bus-off.
- **Bus-off:** by default, restart the channel yourself and report
  `CAN_ERR_RESTARTED`. Mainline Linux cannot restart a gs_usb channel, so this
  is what makes it usable. With `GS_CAN_FEATURE_BUS_OFF_RECOVERY` (bit 18,
  candleLight_fw extension), a host that starts the channel with
  `GS_CAN_FLAG_BUS_OFF_RECOVERY` takes over and triggers recovery with
  `BREQ_BUS_OFF_RECOVERY`.
- **`AUTO_RESTART` (bit 31):** the older private flag. It explicitly asks for
  automatic restart and overrides `GS_CAN_FLAG_BUS_OFF_RECOVERY`. Linux masks
  out bits 18 and 31.

Wire-format notes: request numbers and the frame layout follow the Linux kernel
driver. The `candle_api` comments disagree on some request numbers. A frame
is 20 bytes (classic) or 76 bytes (FD), plus a 4-byte `timestamp_us` once the
host enables `GS_CAN_FLAG_HW_TIMESTAMP`. An FD frame spans two 64-byte bulk
packets.

### lin_usb (LIN)

The device is **self-scheduling**. The host uploads schedule tables (up to
`LIN_USB_MAX_SCHEDULE_TABLES` × `LIN_USB_MAX_SCHEDULE_ENTRIES`, both reported in
`BREQ_DEVICE_CONFIG`) and starts a table with `BREQ_MODE`. From then on the
device runs the bus timing itself.

| Hook | Called for |
| --- | --- |
| `lin_engine_configure(ch, cfg)` | `BREQ_BAUDRATE`: baud rate, LIN version, `LIN_USB_FLAG_MASTER` / `LIN_USB_FLAG_LISTEN_ONLY` |
| `lin_engine_set_mode(ch, mode)` | `BREQ_MODE`: start (with `table_id`), stop, pause |
| `lin_engine_set_schedule_entry(ch, slot, e)` | `BREQ_SCHEDULE`: write one slot of a table |
| `lin_engine_update_frame(ch, e)` | `BREQ_FRAME_CONFIG`: update an existing frame by LIN ID |
| `bool lin_engine_set_data(ch, frame)` | Bulk OUT "set frame data". Return `false` if no publisher slot in the active table has that ID |
| `lin_engine_sleep_wakeup(ch, cmd)` | `BREQ_SLEEP_WAKEUP`: go-to-sleep or wakeup pulse |
| `lin_engine_get_bus_state(ch, st)` | `BREQ_BUS_STATE`: `LIN_USB_BUS_STATE_*` |
| `lin_engine_identify(ch)` | `BREQ_IDENTIFY` |
| `lin_engine_task()` | Called from `lin_usb_task()` |
| `lin_engine_reset()` | USB bus reset or unplug: stop every channel |
| `lin_engine_suspend()` / `lin_engine_resume()` | USB suspend / resume: stop the channels that are running (a master would keep driving its schedule), then restart exactly those with their configuration and schedule. Leave channels the host put to sleep alone. Default: no-op |

Protocol behaviour handled by the transport:

- **Invalid control requests are STALLed.** An out-of-range `table_id`, slot
  or DLC makes the host's control transfer fail (`EPIPE`), so bad input is
  never silently ignored.
- **Every bulk-OUT "set frame data" packet is acknowledged** on bulk IN with
  `echo_id = LIN_USB_ECHO_ID_SET_DATA_ACK` (0). The ACK carries
  `LIN_USB_FRAME_FLAG_ERROR` if the packet was malformed or the ID is not in the
  running schedule. The data itself goes out on that slot's next schedule event.
  If the IN queue is full, the ACK is held and the OUT endpoint NAKs further
  packets until it fits, so an ACK is never lost.
- **Bus events** use `echo_id = LIN_USB_ECHO_ID_RX` and are reported with
  `lin_usb_report_frame()`.
- **Dropped frames:** frames that did not fit into the IN queue are counted per
  channel and returned in the `dropped` field of the `BREQ_BUS_STATE` reply.

`LIN_USB_FRAME_FLAG_WAKEUP` is the new name of bit `0x08`. The old name
`LIN_USB_FRAME_FLAG_TX_UPDATE` is kept as an alias.

### aio_usb (digital I/O + analog)

| Hook | Called for |
| --- | --- |
| `aio_hw_config_io(io, mode, …)` | Configure a line as input or output |
| `aio_hw_set_outputs(mask, values)` | Drive outputs |
| `uint32_t aio_hw_read_inputs(void)` | Sample all digital inputs |
| `uint16_t aio_hw_read_analog(ch)` | Read one analog channel |
| `aio_hw_identify()` | `BREQ_IDENTIFY` |

Size the interface to your board in `aio_usb_config.h`. The line count, the
analog count and resolution, and the per-line input/output capability masks are
all reported to the host.

On USB suspend the transport pauses auto-reports and restarts their timers on
resume. Output levels are left as they are: they generate no traffic, and
changing them because the host went to sleep would be surprising.

All three drivers get suspend/resume from the single `tud_suspend_cb()` /
`tud_resume_cb()` pair in `usb_app_drivers.c`.

## Linux quick test

```bash
sudo ip link set can0 up type can bitrate 500000                    # classic CAN
sudo ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on   # CAN FD
candump can0
cansend can0 123#DEADBEEF
cansend can0 123##1.00112233445566778899AABBCCDDEEFF               # FD frame with BRS
```

With the default stub engines, the interface comes up and accepts frames, but
nothing is sent on the bus and no echo comes back.

## Sample host application

`SampleApp/` is a C++17 host program on libusb-1.0 for Linux and Windows. It
opens all three interfaces at the same time and exercises every request:

| File | Content |
| --- | --- |
| `gs_usb_protocol.h`, `lin_usb_protocol.h`, `aio_usb_protocol.h` | Host-side copies of the wire protocol. Keep them in sync with `Core/Inc/*_usb.h` |
| `CandeApi` | CAN: bit timing (nominal + FD data phase), mode, `BT_CONST_EXT`, `GET_STATE`, termination, bus-off recovery, classic and FD frame I/O |
| `LindeApi` | LIN: bus config, schedule upload/start, sleep/wakeup, bus state. `setFrame()` waits for the device's set-data ACK and keeps bus frames that arrive meanwhile for `getFrame()` |
| `AiodeApi` | AIO: I/O config, outputs, status, auto-report |
| `main.cpp` | Test run with a PASSED/FAILED summary per interface |

```bash
cd SampleApp
cmake -B build && cmake --build build
./build/CanLinAioSample   # Linux: needs access to the USB device (root or a udev rule)
```

On Linux, `CandeApi` detaches the `gs_usb` kernel driver while it runs and
re-attaches it on `close()`. Tests for optional features (FD, loop-back,
GET_STATE, auto-report, …) run only when the device advertises the feature.

## License

The USB class drivers (`gs_usb*`, `lin_usb*`, `aio_usb*`, `usb_app_*`,
`usb_descriptors.c`) and the SampleApp are MIT licensed. See the header of
[gs_usb.c](Core/Src/gs_usb.c) for the full license text.
Copyright (c) 2026 Schildkroet

CubeMX-generated files (`main.c`, `stm32g4xx_*`, `system_stm32g4xx.c`,
startup code, linker scripts) are © STMicroelectronics, under the license
terms in their headers. TinyUSB (`Core/TinyUSB/`) is MIT licensed
(© Ha Thach, tinyusb.org); the license text is in each source file's header.
