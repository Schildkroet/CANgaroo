
# <img src="src/assets/cangaroo.png" width="48" height="48"> CANgaroo
**Open-source CAN bus analyzer for Linux 🐧 / Windows 🪟**

**🔩 Supported Interfaces & Hardware:**

| Interface | Linux | Windows | Notes |
| :--- | :---: | :---: | :--- |
| **SocketCAN** | ✅ | — | Any kernel CAN interface (`can0`, `vcan0`, …) |
| **PEAK PCAN** | ✅ | ✅ | PCAN-USB, PCAN-USB Pro, PCAN-PCIe, … via PCAN-Basic SDK (`CONFIG+=peakcan`) |
| **Kvaser** | ✅ | ✅ | USB/CAN Leaf and other Kvaser devices via CANlib SDK (`CONFIG+=kvaser`) |
| **Vector** | — | ✅ | VN-series and other Vector devices via Qt serialbus (XL Driver Library required at runtime), CAN FD supported |
| **TinyCAN** | ✅ | ✅ | TinyCAN USB adapters via Qt serialbus (enable in Measurement > Driver menu) |
| **Candlelight / CANable / CANnectivity** | ✅ | ✅ | CANable (Candlelight firmware), MKS CANable, cantact, CANnectivity, and other gs_usb devices. Multi-channel devices supported. CAN FD supported. |
| **SLCAN** | ✅ | ✅ | CANable (SLCAN firmware), Arduino CAN shields |
| **CANblaster** | ✅ | ✅ | UDP-based remote CAN via [CANblaster](https://github.com/OpenAutoDiagLabs/CANblaster) |
| **GrIP** | ✅ | ✅ | GrIP protocol |
| **lin_usb (LindeAPI)** | ✅ | ✅ | USB LIN adapter (VID `0x1d50` / PID `0x606f`). Multi-channel. Master, slave, and monitor modes. Hardware LIN scheduling via LDF. |
| **aio_usb (aiode)** | ✅ | ✅ | Digital I/O and analog inputs on the same USB adapter (VID `0x1d50` / PID `0x606f`), controlled from the GPIO Control window |
| **zscanfd** | ✅ | ✅ | Zilogic _USB to CAN Adapter_ and _USB to CAN FD Adapter_ via zscanfd driver (CONFIG+=zscanfd) in windows (zscanfd.dll required at runtime) and Linux support via SocketCAN |

## ⚙️ Features

*   **Real-time CAN/CAN-FD/LIN Decoding**: Support for standard CAN, high-speed CAN-FD, and LIN bus frames.
*   **Wide Hardware Compatibility**: Works with **SocketCAN** (Linux), **PEAK PCAN**, **Kvaser**, **Vector**, **TinyCAN**, **CANable**, **Candlelight**, **SLCAN**, **CANblaster** (UDP), **GrIP**, **lin_usb** (LIN) and **Zilogic USB to CAN FD Adaptor**.
*   **DBC & LDF Database Support**: Load multiple `.dbc` files for CAN signal decoding and `.ldf` files for LIN bus signal decoding.
*   **Powerful Data Visualization**: Integrated Graphing tools supporting Time-series, Scatter charts, Text-based monitoring, and interactive Gauge views with zoom and live tooltips. Supports both CAN and LIN signals.
*   **Advanced Filtering & Logging**: Isolate critical data with live filters and export captures for offline analysis.
*   **Network Rights Management**: Per-network access control for bus interfaces.
*   **Python Scripting**: Built-in script editor with an embedded Python interpreter (via pybind11). Send and receive CAN and LIN messages, decode signals using loaded DBC/LDF files, and automate tasks. Scripts can be started manually or automatically with the measurement. Ready-to-use example scripts are included in the `examples/` directory.
*   **GPIO Control**: Configure digital lines as inputs or outputs, switch outputs and watch input levels and analog values live on aio_usb and GrIP devices.
*   **CAN Gateway**: Forward messages between two CAN interfaces with configurable per-message filter rules. Active during a running measurement.
*   **LIN Control**: Send LIN Sleep/Wakeup commands, switch schedule tables, and issue LIN diagnostic requests and responses (slave node) on LIN-capable interfaces directly from the UI.
*   **Trace Replay**: Replay captured CAN logs (Vector ASC, candump, PCAP, and PCAPng formats) with adjustable speed, per-message RX/TX direction filtering, channel mapping to live interfaces, and optional autoplay with the measurement. Supports classic CAN, CAN-FD, RTR, and error frames.
*   **Multiple Export Formats**: Save traces as Vector ASC, Vector MDF4, Linux candump, PCAP, or PCAPng (Wireshark-compatible).
*   **Modern Workspace**: A clean, dockable userinterface optimized for multi-monitor setups.

<br>![Cangaroo Trace View](src/docs/view.png)<br>

## Languages

* 🇩🇪 German
* 🇺🇸 English
* 🇪🇸 Spain
* 🇨🇳 Chinese

## 🛠️ Building
### 🐧 Linux

#### Install dependencies

| Distribution | Command |
| :--- | :--- |
| **Ubuntu / Debian** | `sudo apt install build-essential qt6-base-dev qt6-charts-dev qt6-serialport-dev qt6-serialbus-dev qt6-svg-dev qt6-tools-dev qt6-l10n-tools libqt6opengl6-dev libnl-3-dev libnl-route-3-dev libusb-1.0-0-dev python3-dev pybind11-dev pkg-config` |
| **Fedora** | `sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtcharts-devel qt6-qtserialport-devel qt6-qtserialbus-devel qt6-qtsvg-devel qt6-qttools-devel libnl3-devel libusb1-devel python3-devel pybind11-devel pkgconfig` |
| **Arch Linux** | `sudo pacman -S base-devel qt6-base qt6-charts qt6-serialport qt6-serialbus qt6-svg qt6-tools libnl libusb python pybind11 pkgconf` |

#### Build:

```bash
qmake6
make -j$(nproc)
```
The binary will be in `bin/cangaroo`.

#### SocketCAN privileges

CANgaroo uses `ip link` to configure SocketCAN interfaces (bitrate, sample point, CAN FD), which requires `CAP_NET_ADMIN`. The recommended way is a targeted sudoers rule so no password prompt appears:

```bash
sudo groupadd cangaroo
sudo usermod -aG cangaroo $USER
```

Create `/etc/sudoers.d/cangaroo`:
```
%cangaroo ALL=(ALL) NOPASSWD: /sbin/ip link set * down, /sbin/ip link set * up type can *
```

Log out and back in for the group membership to take effect. If you prefer not to use a group, you can instead grant `CAP_NET_ADMIN` directly to the `ip` binary (applies to all users):
```bash
sudo setcap cap_net_admin+ep /sbin/ip
```

> **Note:** If the interface is set to *"Configured by OS"* in the setup dialog, CANgaroo will not touch the interface configuration and no elevated privileges are needed.

#### USB device permissions (udev rules)

Devices accessed directly via libusb (gs_usb / Candlelight, lin_usb / LindeAPI, aio_usb) need a udev rule so that regular users can open them without `sudo`.

Create `/etc/udev/rules.d/99-cangaroo.rules`:

```
# gs_usb / Candlelight / CANable (gs_usb firmware)
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606b", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606f", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="ca01", MODE="0666", GROUP="plugdev", TAG+="uaccess"
```

Then reload and re-plug the device:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> **Note:** Your user must be in the `plugdev` group (`sudo usermod -aG plugdev $USER`, then log out and back in).

### 🪟 Windows

* Install [Qt 6](https://www.qt.io/download-qt-installer) (Community / Open Source) including the **Qt Serial Bus** component.
* Install [Python 3](https://www.python.org/downloads/) and [pybind11](https://github.com/pybind/pybind11) (`pip install pybind11`).
* Open `cangaroo.pro` in Qt Creator and build.

#### Deployment

Include the required Qt6 libraries or run `windeployqt` on the `.exe`:
```
windeployqt --release cangaroo.exe
```

### Optional hardware drivers

**PEAK PCAN** (`CONFIG+=peakcan`) — Windows only:
  1. Download [PCAN-Basic SDK](https://www.peak-system.com/fileadmin/media/files/PCAN-Basic.zip) and extract to `src/driver/PeakCanDriver/pcan-basic-api/`.
  2. Build with `qmake CONFIG+=peakcan` (or add `peakcan` to the Qt Creator qmake arguments).
  3. Place `PCANBasic.dll` (from `pcan-basic-api/x64/`) next to the built `.exe`.

**Kvaser** (`CONFIG+=kvaser`) — Linux and Windows:

  *Linux:*
  1. Download and build [linuxcan](https://www.kvaser.com/downloads-kvaser/) (V5.51.461 or newer):
     ```bash
     tar -xf linuxcan.tar.gz
     make -C linuxcan/canlib
     sudo make -C linuxcan/canlib install
     sudo ldconfig
     ```
  2. Build with `qmake6 CONFIG+=kvaser`.

  *Windows:*
  1. Install the [Kvaser CANlib SDK](https://www.kvaser.com/downloads-kvaser/) (V5.51.461 or newer).
  2. Build with `qmake CONFIG+=kvaser CANLIB_DIR="C:/path/to/Kvaser/Canlib"`.
  3. Place `canlib32.dll` (from `Canlib/Bin/`) next to the built `.exe`.

**Vector** (always enabled) — Windows only:
  * Install the [Vector XL Driver Library](https://www.vector.com/int/en/products/products-a-z/libraries-drivers/xl-driver-library/) on the target machine.
  * No build-time SDK needed — Qt's `serialbus` module handles the integration.

**TinyCAN** (toggle in Measurement > Driver menu) — Linux and Windows:
  * Install the [TinyCAN](https://www.mhs-elektronik.de/) driver/library on the target machine.
  * No build-time SDK needed — Qt's `serialbus` module handles the integration.
  * Enable the driver via **Measurement > Driver > TinyCAN** and restart the application.

**Zilogic USB to CAN FD Adaptor** (`CONFIG+=zscanfd`) — Windows:
  1. Download the [zscanfd.dll device driver](The download link has been added to the `src.pro` file) on the target machine.
  2. Download the [qtzscanfdbus.dll Qt plugin](The download link has been added to the `src.pro` file) on the target machine.
  3. Build with `qmake CONFIG+=zscanfd` (or add `zscanfd` to the Qt Creator qmake arguments).
  4. Place the `qtzscanfdbus.dll` from `plugin/canbus`
  5. Place the `zscanfd.dll` from `bin/cangaroo`

## Reference adapter firmware

[`firmware/STM32G4_TinyUSB_CanLinAio/`](firmware/STM32G4_TinyUSB_CanLinAio/README.md)
is a bare STM32CubeIDE project (STM32G473, TinyUSB) for building your own
adapter. It implements the device side of all three USB interfaces CANgaroo
talks to: **gs_usb** (CAN / CAN FD), **lin_usb** (LIN) and **aio_usb** (I/O +
analog). It contains only the USB transport. Plug your CAN, LIN and GPIO code
into its weak `gs_engine_*`, `lin_engine_*` and `aio_hw_*` hooks. `SampleApp/`
inside it is a standalone libusb host program that exercises every request.
The wire protocol is documented in [`src/docs/usb_interfaces.md`](src/docs/usb_interfaces.md).

## ARXML to DBC Conversion

Cangaroo natively supports DBC. If you have ARXML files, you can convert them using `canconvert`:
```bash
# Install canconvert
pip install canconvert

# Convert ARXML to DBC
canconvert TCU.arxml TCU.dbc
```

## 📥 Download

Download the latest release from the [Releases](https://github.com/Schildkroet/CANgaroo).

## 📜 Credits

Written by Hubert Denkmair <hubert@denkmair.de>

Further development by:
* Ethan Zonca <e@ethanzonca.com>
* WeAct Studio
* Schildkroet (https://github.com/Schildkroet/CANgaroo)
* Wikilift (https://github.com/wikilift/CANgaroo)
* Jayachandran Dharuman (https://github.com/OpenAutoDiagLabs/cangaroo)

## DISCLAIMER

This software is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and non-infringement. In no event shall the authors, maintainers, contributors, or copyright holders be liable for any claim, damages, or other liability, whether in an action of contract, tort, or otherwise, arising from, out of, or in connection with the software or the use or other dealings in the software.
Use of this software is entirely at your own risk. The authors and maintainers accept no responsibility for any harm, data loss, system damage, legal issues, or any other consequences resulting from the use, misuse, or inability to use this software. It is your sole responsibility to ensure that this software is suitable for your intended use case and complies with all applicable laws and regulations in your jurisdiction.
This project is not affiliated with, endorsed by, or in any way officially connected to any third-party organizations, products, or services that may be referenced within it.
