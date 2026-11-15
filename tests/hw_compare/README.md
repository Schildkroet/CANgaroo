# slcan_hw_compare

Checks `FakeSlcanDevice` against a real SLCAN adapter. Not part of `make check`:
it needs hardware and Qt SerialPort.

```bash
cd tests/hw_compare && qmake6 && make
B=../../build/tests/slcan_hw_compare
$B probe /dev/ttyACM0 > real.txt; $B probe fake:canable2 > fake.txt; diff real.txt fake.txt
$B driver /dev/ttyACM0 canable2; $B driver fake:canable2
$B bus /dev/ttyACM0 can0      # a SocketCAN channel on the same bus, 500k / 2M FD
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
