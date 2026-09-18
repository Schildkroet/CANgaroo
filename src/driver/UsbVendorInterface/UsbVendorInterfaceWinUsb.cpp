/*

  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "UsbVendorInterface.h"

#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <winusb.h>

namespace
{
constexpr UCHAR BRT_VENDOR_ITF_OUT = 0x41u; // vendor | interface | host->device
constexpr UCHAR BRT_VENDOR_ITF_IN  = 0xC1u; // vendor | interface | device->host

constexpr int SLOT_CONTROL = 0;
constexpr int SLOT_IN      = 1;
constexpr int SLOT_OUT     = 2;

bool parseGuid(const char *text, GUID &guid)
{
    if (!text)
        return false;
    const std::wstring wide(text, text + std::char_traits<char>::length(text));
    return SUCCEEDED(CLSIDFromString(wide.c_str(), &guid));
}

// Device paths of all present device nodes exposing the interface GUID the
// firmware sets for this interface in its MS OS 2.0 descriptor.
std::vector<std::wstring> interfacePaths(const UsbVendorInterface::Id &id)
{
    std::vector<std::wstring> paths;
    GUID guid{};
    if (!parseGuid(id.windowsGuid, guid))
        return paths;

    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        return paths;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &ifData); i++)
    {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifData, nullptr, 0, &size, nullptr);
        if (size == 0)
            continue;

        std::vector<BYTE> buf(size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(info, &ifData, detail, size, nullptr, nullptr))
            paths.emplace_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(info);
    return paths;
}

UsbVendorInterface::Status toStatus(DWORD err)
{
    switch (err)
    {
    case ERROR_SEM_TIMEOUT:
        return UsbVendorInterface::Status::Timeout;
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_BAD_COMMAND:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_HANDLE:
        return UsbVendorInterface::Status::NoDevice;
    default:
        return UsbVendorInterface::Status::Error;
    }
}

std::string errorText(DWORD err)
{
    char *msg = nullptr;
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, err, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string text = len ? std::string(msg, len) : std::string("error ") + std::to_string(err);
    if (msg)
        LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}
} // namespace

UsbVendorInterface::~UsbVendorInterface()
{
    close();
}

int UsbVendorInterface::count(const Id &id)
{
    return static_cast<int>(interfacePaths(id).size());
}

bool UsbVendorInterface::open(const Id &id, int index)
{
    close();

    GUID guid{};
    if (!parseGuid(id.windowsGuid, guid))
    {
        fail(Status::Error, "open", std::string("invalid interface GUID ") + (id.windowsGuid ? id.windowsGuid : "(null)"));
        return false;
    }

    const std::vector<std::wstring> paths = interfacePaths(id);
    if (index < 0 || index >= static_cast<int>(paths.size()))
    {
        fail(Status::NoDevice, "open",
             std::string("no device with interface GUID ") + id.windowsGuid + " at index " + std::to_string(index)
                 + ". Check in Device Manager that the interface uses the WinUSB driver.");
        return false;
    }

    // Opens only this interface's device node, so sibling interfaces held by
    // other drivers (or other programs) do not get in the way.
    HANDLE file = CreateFileW(paths[static_cast<size_t>(index)].c_str(),
                              GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_WRITE | FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            fail(Status::Error, "open", "interface is in use by another program");
        else
            fail(toStatus(err), "CreateFile", errorText(err));
        return false;
    }
    _file = file;

    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    if (!WinUsb_Initialize(file, &winusb))
    {
        const DWORD err = GetLastError();
        fail(toStatus(err), "WinUsb_Initialize", errorText(err));
        close();
        return false;
    }
    _winusb = winusb;

    USB_INTERFACE_DESCRIPTOR desc{};
    if (!WinUsb_QueryInterfaceSettings(winusb, 0, &desc))
    {
        const DWORD err = GetLastError();
        fail(toStatus(err), "WinUsb_QueryInterfaceSettings", errorText(err));
        close();
        return false;
    }
    if (desc.bInterfaceProtocol != id.protocol)
    {
        fail(Status::Error, "open",
             "interface protocol " + std::to_string(desc.bInterfaceProtocol) + " does not match the expected "
                 + std::to_string(id.protocol) + " (wrong DeviceInterfaceGUID in the firmware?)");
        close();
        return false;
    }
    _itf = desc.bInterfaceNumber;

    for (UCHAR e = 0; e < desc.bNumEndpoints; e++)
    {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(winusb, 0, e, &pipe) || pipe.PipeType != UsbdPipeTypeBulk)
            continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId))
            _epIn = pipe.PipeId;
        else
            _epOut = pipe.PipeId;
    }
    return true;
}

void UsbVendorInterface::close()
{
    if (_winusb)
    {
        WinUsb_Free(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb));
        _winusb = nullptr;
    }
    if (_file)
    {
        CloseHandle(static_cast<HANDLE>(_file));
        _file = nullptr;
    }
    for (auto &t : _pipeTimeout)
        t.store(0);
    _itf   = 0;
    _epIn  = 0;
    _epOut = 0;
}

bool UsbVendorInterface::isOpen() const
{
    return _winusb != nullptr;
}

bool UsbVendorInterface::setPipeTimeout(int slot, uint8_t pipeId, unsigned timeout_ms)
{
    // Each pipe is used by one thread at a time, so the cached value only saves
    // a policy call per transfer.
    if (_pipeTimeout[slot].load() == timeout_ms)
        return true;
    ULONG value = timeout_ms;
    if (!WinUsb_SetPipePolicy(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb), pipeId,
                              PIPE_TRANSFER_TIMEOUT, sizeof(value), &value))
    {
        return false;
    }
    _pipeTimeout[slot].store(timeout_ms);
    return true;
}

UsbVendorInterface::Status UsbVendorInterface::bulkRead(void *data, int len, int &transferred, unsigned timeout_ms)
{
    transferred = 0;
    if (!_winusb || !_epIn)
        return fail(Status::Error, "bulk IN", "device not open");
    setPipeTimeout(SLOT_IN, _epIn, timeout_ms);

    ULONG got = 0;
    if (!WinUsb_ReadPipe(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb), _epIn,
                         static_cast<PUCHAR>(data), static_cast<ULONG>(len), &got, nullptr))
    {
        const DWORD err = GetLastError();
        // Timeouts are routine for the polling reader: not an error to report.
        if (err == ERROR_SEM_TIMEOUT)
            return Status::Timeout;
        return fail(toStatus(err), "bulk IN", errorText(err));
    }
    transferred = static_cast<int>(got);
    return Status::Ok;
}

UsbVendorInterface::Status UsbVendorInterface::bulkWrite(const void *data, int len, unsigned timeout_ms)
{
    if (!_winusb || !_epOut)
        return fail(Status::Error, "bulk OUT", "device not open");
    setPipeTimeout(SLOT_OUT, _epOut, timeout_ms);

    ULONG sent = 0;
    // WinUSB takes a non-const buffer but does not modify it for OUT transfers.
    auto *buf = static_cast<PUCHAR>(const_cast<void *>(data));
    if (!WinUsb_WritePipe(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb), _epOut,
                          buf, static_cast<ULONG>(len), &sent, nullptr))
    {
        const DWORD err = GetLastError();
        return fail(toStatus(err), "bulk OUT", errorText(err));
    }
    if (sent != static_cast<ULONG>(len))
        return fail(Status::Error, "bulk OUT", "short write");
    return Status::Ok;
}

UsbVendorInterface::Status UsbVendorInterface::controlIn(uint8_t bRequest, uint16_t wValue,
                                                         void *data, uint16_t len, unsigned timeout_ms)
{
    if (!_winusb)
        return fail(Status::Error, "control IN", "device not open");
    setPipeTimeout(SLOT_CONTROL, 0, timeout_ms);

    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = BRT_VENDOR_ITF_IN;
    setup.Request     = bRequest;
    setup.Value       = wValue;
    setup.Index       = _itf;
    setup.Length      = len;
    ULONG got = 0;
    if (!WinUsb_ControlTransfer(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb), setup,
                                static_cast<PUCHAR>(data), len, &got, nullptr))
    {
        const DWORD err = GetLastError();
        return fail(toStatus(err), "control IN", errorText(err));
    }
    return Status::Ok;
}

UsbVendorInterface::Status UsbVendorInterface::controlOut(uint8_t bRequest, uint16_t wValue,
                                                          const void *data, uint16_t len, unsigned timeout_ms)
{
    if (!_winusb)
        return fail(Status::Error, "control OUT", "device not open");
    setPipeTimeout(SLOT_CONTROL, 0, timeout_ms);

    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = BRT_VENDOR_ITF_OUT;
    setup.Request     = bRequest;
    setup.Value       = wValue;
    setup.Index       = _itf;
    setup.Length      = len;
    ULONG sent = 0;
    auto *buf = static_cast<PUCHAR>(const_cast<void *>(data));
    if (!WinUsb_ControlTransfer(static_cast<WINUSB_INTERFACE_HANDLE>(_winusb), setup,
                                buf, len, &sent, nullptr))
    {
        const DWORD err = GetLastError();
        return fail(toStatus(err), "control OUT", errorText(err));
    }
    return Status::Ok;
}
