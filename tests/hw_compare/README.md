# slcan_hw_compare

Checks `FakeSlcanDevice` against a real SLCAN adapter. Not part of `make check`:
it needs hardware and Qt SerialPort.

```bash
cd tests/hw_compare && qmake6 && make
B=../../build/tests/slcan_hw_compare
$B probe /dev/ttyACM0 > real.txt; $B probe fake:canable2 > fake.txt; diff real.txt fake.txt
$B driver /dev/ttyACM0 canable2; $B driver fake:canable2
$B bus /dev/ttyACM0 can0 [elmue]  # a SocketCAN channel on the same bus, 500k / 2M FD

# gs_usb (CandleAPI driver) through libusb; only the adapter with this USB
# serial is touched, its kernel gs_usb driver gets it back afterwards
$B candle-probe 206E38A5505650082
$B candle-bus   206E38A5505650082 can0
$B candle-leave 206E38A5505650082   # leave channel 0 running at 250 kbit/s
```

`probe` sends the same command scripts to both and prints every reply byte.
`driver` runs `SLCANInterface` itself (open, one TX, close) on both.
`bus` sends every frame type from the SocketCAN side and compares the line the
device prints with the fake's; then sends every type through `SLCANInterface`
and compares what reaches the wire with what reaches the fake.

Power-cycle the adapter before `probe`: the CANable error register (`E`) is
sticky, and the fake starts from a reset.

## Results

CANable 2.0 nano (STM32G431), upstream `canable2-fw` b158aa7 built with
arm-gnu-toolchain 13.2, flashed over DFU. Bus partner: one gs_usb FD channel
(SocketCAN `can0`, 500k / 2M), nothing else on the bus.

| Checked | Result |
|---|---|
| Reply to every config command (`C O S Y M A H`, bad ones too) | none, as in the fake |
| `V` | `b158aa7 github.com/normaldotcom/canable2-fw.git\r`; the fake had assumed `https://` (fixed) |
| Empty line | repeats the previous command; the fake ignored it (fixed: `slcan_str` is kept between lines) |
| Frame while closed | dropped, sets `ERR_CAN_TXFAIL` (`E` shows 4, sticky until power-off); now modelled |
| `SLCANInterface` open / TX / close | identical, incl. no-confirm detection and ~110 ms open |
| RX of std / ext / remote / FD / FD+BRS / 64-byte ext FD | identical lines |
| Remote frame RX data digits | undefined: stale RX FIFO bytes that change between runs (`r45641122337F`); fake reports zeros, harness compares up to the DLC |
| TX of the same frame types via `SLCANInterface` | identical on the wire, incl. `B` for extended FD and remote frames without data digits |
| `M1` then `O` (listen only) | ignored: `can0`'s frames are ACKed, its TX error counter stays 0 |
| Old confirm probe under traffic | misdetected confirm mode in 1/100 opens at 2 ms frame gap, 26/100 at full load; the fake's `leaveRunning()` is the worst case (always). Fixed driver: 30/30 correct at full load |

Not covered: bitrates other than 500k / 2M (changing the SocketCAN side needs
root), bus errors, CANable 1 and WeAct firmwares (other MCUs: STM32F042,
STM32G0B1).

## Results: ElmueSoft CANable 2.5 Slcan

Same board, Elmue's Slcan firmware built from
github.com/Elmue/CANable-2.5-firmware-Slcan-and-Candlelight 12aa9fa
(`Build/Make_G431_Slcan_Multiboard`, Slcan 105, firmware 26.10.01).

| Checked | Result |
|---|---|
| 42 probe scripts (incl. open without bitrate, `Y3`, `S7`, `Y8`, silent TX, FD without data rate, short / long / out-of-range frame commands) | identical to `fake:elmue` |
| `V` | ~200 bytes over several USB packets; read in full now (was cut after the first packet) |
| `SLCANInterface` open / TX / close | identical, incl. confirm mode (Elmue's `C` never answers, so the probe alone would say "no confirm") |
| RX and TX of all frame types on the wire | identical, after the sample point fix below |
| Listen only | `M1` works; a frame then gets BEL: TX error, nothing on the wire, as in the fake |
| Open under full bus load | 20/20 correct |

Found on the wire: BRS frames sent by Elmue failed (`can0` RX errors up to
128) with the `S6` preset, which samples at 75 % against the bus's 87.5 %.
With `s2,139,20,…` (87.5 %) they went through ("Perfect match: Yes" in Elmue's
debug output). The driver now sets Elmue's timing with `s`/`y` from the
configured bitrate and sample point (`SlcanBitTiming.h`), using the clock and
limits from the `V` reply.

## Results: gs_usb, ElmueSoft Candlelight 2.5 (CandleAPI driver)

Same board with Elmue's Candlelight firmware (`Build/Make_G431_Candle_Multiboard`,
26.10.01), against `FakeGsUsbDevice` (Elmue G431). candle.c runs on Linux here
through `candle_usb_hw.cpp` (libusb) instead of WinUSB.

| Checked | Result |
|---|---|
| 28 control requests (config, bt_const, bt_const_ext, state, refused requests, START without / with bad / with valid timing, timing and START while running) | identical, byte for byte apart from HAL version and timestamp values |
| `CandleApiInterface` RX and TX of all frame types on the wire (`candle-bus`) | identical, incl. 64-byte BRS frames, every TX shown once |
| Channel left running at 250k, then opened at 500k | fixed driver: reconfigured, 0 differences. Without the fix (no RESET before the bit timing): stays at 250k, 16 differences |

The legacy gs_usb echo comes when a frame is queued, not when it was ACKed:
with the device at the wrong bitrate, frames were shown as sent that never
reached the bus. That is the firmware's protocol (Elmue's comment calls the echo
"useless"), not something the driver can see.

