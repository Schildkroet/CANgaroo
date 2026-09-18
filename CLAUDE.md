# CANgaroo

CAN bus analyzer and trace tool built with Qt6 and C++.

## Build System

- **qmake** (not CMake) with `.pro` / `.pri` files
- Entry point: `src/src.pro`
- Requires **Qt 6** (Widgets, Xml, Charts, SerialPort, SerialBus, Network)
- Requires **libusb-1.0** via pkg-config on Linux/macOS (LindeAPI and aiode drivers are always built); Windows uses WinUSB instead
- Version is defined in `src/src.pro` via `VERSION = x.y.z`

### Build (Linux)

```bash
cd src && qmake6 && make -j$(nproc)
```

### Tests

```bash
cd tests && qmake6 && make -j$(nproc) && make check
```

Needs Qt Test (`qt6-base-dev`). Also reachable from the top level via
`qmake6 CONFIG+=tests` — opt-in so the normal build has no testlib dependency.
Run by the `unit-tests` CI job. Binaries land in `build/tests/`.

One binary per area, each linking only the sources it exercises (never `Backend`,
which would drag in drivers and the Python engine). `tests/common.pri` holds the
shared config; `tests/support/LogStub.cpp` satisfies `core/Log` without `Backend`.

| Directory | Covers |
|-----------|--------|
| `bus_message_signal/` | signal packing: `extractRawSignal` / `injectRawSignal` |
| `bus_message_frame/`  | DLC, identifier/flag packing, accessor bounds |
| `can_db_signal/`      | raw <-> physical, sign extension, multiplexer gate |
| `dbc_parser/`         | DBC text to `CanDb`, incl. Motorola start-bit conversion |
| `can_db/`             | `CanDb::updateFrom` (DBC reload), muxer wiring |
| `trace_file_format/`  | export format resolution from extension / name |
| `trace_line_format/`  | ASC / candump lines, CAN FD DLC table, ASC CAN FD parsing (vectors from python-can) |
| `pcapng_format/`      | pcapng SHB / IDB / EPB bytes for SocketCAN frames (from the pcapng spec + `linux/can.h`) |
| `trace_file_writer/`  | whole export files in every format (`TraceFileWriter`), incl. MDF4 block structure |
| `trace_recorder/`     | `TraceRecorder` arming, naming, splitting, per-part file structure |
| `decoders/`           | UDS and J1939 (adopted from the old `src/decoders/test/`) |
| `autosar_e2e/`        | E2E Profile 2 CRC-8H2F |
| `ldf_parser/`         | LIN Description File parsing |
| `slcan_codec/`        | SLCAN ASCII frame decoding, incl. malformed input |

**Expected values come from outside cangaroo.** The big-endian bug in issue #34
survived because extract and inject were wrong in mutually cancelling ways, so
round-trip assertions written against cangaroo alone passed. DBC signal vectors
are generated with **cantools**; the E2E CRC is anchored on the AUTOSAR spec check
value plus a bitwise reimplementation. Never generate vectors from cangaroo output.

**Trace file compatibility** is checked against independent readers by
`tests/format_compat/` (CI job `format-compat`, not part of `make check`):
`trace_format_samples` writes every export format plus split recorder output,
`validate.py` reads them with python-can (candump, ASC), scapy (pcap, pcapng) and
asammdf (MDF4). Frames are defined in both files on purpose; reader warnings fail.

```bash
cd tests/format_compat && qmake6 && make && cd ../..
python3 -m venv .venv && .venv/bin/pip install -r tests/format_compat/requirements.txt
build/tests/trace_format_samples build/format_compat
.venv/bin/python tests/format_compat/validate.py build/format_compat
```

Don't use C++14 digit separators (`1'000`) in test `.cpp` files with `Q_OBJECT`:
qmake's moc scanner reads `'` as a character literal, misses `Q_OBJECT`, and the
build fails with a missing `<Name>.moc`.

Known-bad behaviour is marked with `QEXPECT_FAIL` plus a comment explaining the
fix, never asserted as if correct. When the bug is fixed the test reports
"expected failure but passed" - drop the marker and assert the real behaviour.

### Optional driver flags

```bash
qmake6 CONFIG+=kvaser CONFIG+=peakcan
```

- `CONFIG+=peakcan` — enables PeakCAN driver (Windows only, needs PCAN-Basic SDK)
- `CONFIG+=kvaser` — enables Kvaser driver (needs CANlib SDK)

### Platform-specific drivers

| Driver        | Platform | Notes                                      |
|---------------|----------|--------------------------------------------|
| SocketCAN     | Linux    | libnl-3, libnl-route-3                     |
| SLCAN         | All      | Serial port based                          |
| GrIP          | All      | Serial port based                          |
| CANBlaster    | All      | UDP based                                  |
| CandleAPI     | Windows  | gs_usb devices                             |
| LindeAPI      | All      | lin_usb LIN adapter, via `UsbVendorInterface` |
| PeakCAN       | Windows  | Requires `CONFIG+=peakcan`                 |
| Kvaser        | All      | Requires `CONFIG+=kvaser` + CANlib SDK     |
| Vector        | All      | Qt SerialBus plugin (`vectorcan`)          |
| TinyCAN       | All      | Qt SerialBus plugin (`tinycan`)            |

## Architecture

### Driver pattern

- `CanDriver` — discovers and owns `BusInterface` instances
- `BusInterface` (QObject) — abstract interface for send/receive; LIN-capable interfaces implement `sendLinSleepWakeup()` and advertise `capability_lin_master/slave/monitor` flags
- `BusListener` — runs in a dedicated QThread, calls `readMessage()` in a loop and feeds `BusTrace`
- Driver constructors use `reinterpret_cast<CanDriver*>(driver)`
- TX messages: appended to a mutex-protected `_txMsgList` in `sendMessage()`, dequeued in `readMessage()`
- Vendor USB interfaces of the composite 1d50:606f adapter (lin_usb, aio_usb) go through
  `driver/UsbVendorInterface`: libusb on Linux/macOS, WinUSB by `DeviceInterfaceGUID` on Windows.
  Never `libusb_open()` it on Windows: that opens every WinUSB interface and collides with
  `CandleApiDriver` on gs_usb interface 0 (see `src/docs/usb_interfaces.md`)
- Qt SerialBus plugins (Vector, TinyCAN): check `QCanBus::instance()->plugins().contains()` before use
- Drivers with enable/disable toggle (CANBlaster, TinyCAN): follow settings pattern in `mainwindow.cpp`

### Key classes

- `Backend` — singleton, owns `BusTrace`, measurement setup, drivers
- `BusTrace` — thread-safe message store, supports save to candump/ASC/MDF4/PCAP/PCAPng
  via `save(QFile&, TraceFileFormat)`; the format mapping lives in `core/TraceFileFormat.h`
  and is shared by the GUI save dialog and the Python `save_trace()` binding
- `TraceRecorder` — owned by `Backend`; streams every frame to ASC/candump/PCAPng files while armed
  (Measurement → Record, Ctrl+R), independent of `BusTrace`'s in-memory size limit. Fed from
  `BusTrace::enqueueMessage` (listener threads, queue only); writes on the main thread in batches.
  Encoding is shared with the one-shot export via `core/TraceLineFormat` and `core/PcapNgFormat`.
  Takes an interface-name function instead of `Backend`, so it is unit-testable
- `TraceFileWriter` — Backend-free writers for whole export files (candump, ASC, MDF4, PCAP,
  PCAPng); `BusTrace::save()` delegates here. `core/SocketCan.h` maps a `BusMessage` to
  `canid_t` and error classes (`linux/can/error.h`) for every binary/candump format
- `CanMessage` — value type representing a single CAN frame (registered as Qt metatype)
- `BusMessage` — unified CAN/LIN message type; `BusMessage::Type` enum distinguishes `CAN` vs `LIN`

### File layout

```
src/
  core/          — Backend, BusTrace, CanMessage, CanDb, Log, MeasurementSetup
  driver/        — BusInterface, BusListener, CanDriver + per-driver subdirectories
  parser/        — dbc/ (DBC parser), ldf/ (LIN Description File parser, header-only)
  decoders/      — protocol decoders (UDS, J1939)
  window/        — UI windows (TraceWindow, SetupDialog, TxGeneratorWindow, ReplayWindow, GatewayWindow, LinControlWindow, ...)
  helpers/       — utility code
  mainwindow.*   — application shell, driver registration, menu actions
examples/        — Python scripting example scripts
tests/           — Qt Test unit tests (opt-in build)
```

### Adding a new driver

1. Create `src/driver/NewDriver/` with `NewDriver.h/.cpp`, `NewInterface.h/.cpp`, `NewDriver.pri`
2. In `.pri`: list HEADERS/SOURCES, add `QT += serialbus` if using Qt plugin
3. Include the `.pri` from `src/src.pro` (use `win32:` or `unix:` prefix if platform-specific)
4. Register the driver in `mainwindow.cpp` (with optional settings-based enable/disable)
5. TX reporting: add `QMutex _txMutex` + `QList<CanMessage> _txMsgList` to the interface

## Code Conventions

- Use modern C++ (C++20 and above):
- `auto` and structured bindings where they improve readability
- Range-based for loops, `std::as_const()` for const iteration
- `constexpr` and `if constexpr` where applicable
- Smart pointers (`std::unique_ptr`, `std::shared_ptr`) over raw owning pointers
- `std::chrono` for all time-related code
- Concepts and constraints for template interfaces
- Designated initializers for aggregate types
- `std::optional`, `std::variant`, `std::string_view` where appropriate
- `[[nodiscard]]`, `[[maybe_unused]]` attributes
- `noexcept` on move constructors, destructors, and non-throwing functions
- Scoped enums (`enum class`) over unscoped enums
- Allman brace style (opening brace on its own line)
- Qt6 API only (no deprecated Qt5 patterns)
- `override` instead of `Q_DECL_OVERRIDE`
- `QString::arg()` / `QString::number()` instead of `QString::asprintf()`
- `static_cast` / `reinterpret_cast` instead of C-style casts
- `QProcess::start()` + `waitForFinished()` instead of `QProcess::execute()`
- Include order: own header, C++ standard, Qt, project, platform
- Separate include groups with blank lines

## Theming

- `ThemeManager` (`core/`) applies the app's Light/Dark palette + QSS (`assets/{light,dark}_theme.qss`).
- Setting `ui/nativeStyling` (bool, default **true**, toggled via Settings → Appearance) makes `ThemeManager::applyTheme()` skip the custom palette/QSS and defer to the active `QStyle` + desktop platform theme — for a native GNOME/Adwaita look. The non-native path keeps the bundled Fusion-style Light/Dark theme.
- Start/Stop accent buttons are styled per-widget (`MainWindow::applyControlButtonStyles`), not via the global QSS, so the accent survives native mode.
- Toolbar/menu icons use `QIcon::fromTheme(name, fallback)` (`MainWindow::applyActionIcons`) to match the system icon theme.
- On GNOME/Wayland, `main.cpp` sets `QT_QPA_PLATFORMTHEME=gnome` and `QT_WAYLAND_DECORATION=adwaita` when unset. The full native look needs a Qt6 Adwaita style (provides the `libadwaita`/`adwaita` style keys), the `qadwaitadecorations` plugin, and a GNOME Qt platform theme installed.
- `MainWindow` follows live desktop Light/Dark switches via `QStyleHints::colorSchemeChanged` (Qt 6.5+).

## CI

- GitHub Actions workflow: `.github/workflows/cmake.yml`
- Jobs `unit-tests` (Qt Test) and `format-compat` (trace files vs. python-can / scapy / asammdf)
- MSYS2 for Windows builds with built-in caching (`cache: true` in setup-msys2)
- Version extracted from `src/src.pro` (not git describe)
