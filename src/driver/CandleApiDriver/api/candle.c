/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>

  This file is part of the candle windows API.

  This library is free software: you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "candle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "candle_defs.h"
#include "candle_ctrl_req.h"
#include "ch_9.h"

static bool candle_dev_interal_open(candle_handle hdev);

candle_log_fn_t candle_log_fn = NULL;
bool candle_log_verbose = false;

static void candle_logf(const wchar_t *fmt, ...)
{
    if (candle_log_fn == NULL) {
        return;
    }

    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    HRESULT hr = StringCchVPrintfW(buf, 512, fmt, args);
    va_end(args);

    if (SUCCEEDED(hr)) {
        candle_log_fn(buf);
    }
}

static void candle_logf_verbose(const wchar_t *fmt, ...)
{
    if (candle_log_fn == NULL || !candle_log_verbose) {
        return;
    }

    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    HRESULT hr = StringCchVPrintfW(buf, 512, fmt, args);
    va_end(args);

    if (SUCCEEDED(hr)) {
        candle_log_fn(buf);
    }
}

static bool candle_read_di(HDEVINFO hdi, SP_DEVICE_INTERFACE_DATA interfaceData, candle_device_t *dev)
{
    /* get required length first (this call always fails with an error) */
    ULONG requiredLength=0;
    SetupDiGetDeviceInterfaceDetail(hdi, &interfaceData, NULL, 0, &requiredLength, NULL);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        dev->last_error = CANDLE_ERR_SETUPDI_IF_DETAILS;
        return false;
    }

    PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA) LocalAlloc(LMEM_FIXED, requiredLength);

    if (detail_data != NULL) {
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    } else {
        dev->last_error = CANDLE_ERR_MALLOC;
        return false;
    }

    bool retval = true;
    ULONG length = requiredLength;
    if (!SetupDiGetDeviceInterfaceDetail(hdi, &interfaceData, detail_data, length, &requiredLength, NULL) ) {
        dev->last_error = CANDLE_ERR_SETUPDI_IF_DETAILS2;
        retval = false;
    } else if (FAILED(StringCchCopy(dev->path, sizeof(dev->path), detail_data->DevicePath))) {
        dev->last_error = CANDLE_ERR_PATH_LEN;
        retval = false;
    }

    LocalFree(detail_data);

    if (!retval) {
        return false;
    }

    /* try to open to read device infos and see if it is avail */
    if (candle_dev_interal_open(dev)) {
        dev->state = CANDLE_DEVSTATE_AVAIL;
        candle_dev_close(dev);
    } else {
        dev->state = CANDLE_DEVSTATE_INUSE;
    }

    dev->last_error = CANDLE_ERR_OK;
    return true;
}

/* Return true when path already appears in l->dev[0..count-1]. */
static bool candle_path_exists(const candle_list_t *l, unsigned count, const wchar_t *path)
{
    for (unsigned i = 0; i < count; i++) {
        if (wcscmp(l->dev[i].path, path) == 0)
            return true;
    }
    return false;
}

/* Scan one GUID and append found devices to l->dev[] starting at offset.
 * Returns the number of devices appended, or -1 on a hard error (l->last_error set). */
static int candle_scan_guid(candle_list_t *l, const wchar_t *guid_str, unsigned offset)
{
    GUID guid;
    if (CLSIDFromString(guid_str, &guid) != NOERROR) {
        l->last_error = CANDLE_ERR_CLSID;
        return -1;
    }

    HDEVINFO hdi = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) {
        /* No devices with this GUID present — not a hard error. */
        return 0;
    }

    int found = 0;
    for (unsigned i = 0; (offset + i) < CANDLE_MAX_DEVICES; i++) {
        SP_DEVICE_INTERFACE_DATA interfaceData;
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        if (!SetupDiEnumDeviceInterfaces(hdi, NULL, &guid, i, &interfaceData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                l->last_error = CANDLE_ERR_SETUPDI_IF_ENUM;
                found = -1;
            }
            break;
        }

        if (!candle_read_di(hdi, interfaceData, &l->dev[offset + i])) {
            l->last_error = l->dev[offset + i].last_error;
            found = -1;
            break;
        }
        found++;
    }

    SetupDiDestroyDeviceInfoList(hdi);
    return found;
}

/* Scan for WinUSB devices matching vid:pid whose device interface GUID was not
 * covered by the GUID list above.  For each matching USB device instance the
 * function reads DeviceInterfaceGUIDs (or DeviceInterfaceGUID) from the Windows
 * registry, re-uses candle_scan_guid() for each GUID found there, and appends
 * only those devices that are not already present in l->dev[0..existing-1].
 * Returns the number of new devices added. */
static int candle_scan_vidpid(candle_list_t *l, uint16_t vid, uint16_t pid, unsigned existing)
{
    wchar_t hwid_prefix[32];
    _snwprintf(hwid_prefix, 32, L"USB\\VID_%04X&PID_%04X", vid, pid);

    /* Enumerate USB device instances (not interfaces) so we can read hardware IDs. */
    HDEVINFO hdi = SetupDiGetClassDevs(NULL, L"USB", NULL,
                                       DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hdi == INVALID_HANDLE_VALUE) {
        return 0;
    }

    int added = 0;
    SP_DEVINFO_DATA devInfo;
    devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0;
         SetupDiEnumDeviceInfo(hdi, i, &devInfo) && existing + added < CANDLE_MAX_DEVICES;
         i++)
    {

        /* Hardware IDs are a REG_MULTI_SZ — check each string for our VID/PID prefix. */
        wchar_t hwids[512];
        memset(hwids, 0, sizeof(hwids));
        if (!SetupDiGetDeviceRegistryPropertyW(hdi, &devInfo, SPDRP_HARDWAREID,
                NULL, (PBYTE)hwids, sizeof(hwids) - sizeof(wchar_t), NULL)) {
            continue;
        }

        bool matches = false;
        const wchar_t *p;
        for (p = hwids; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, hwid_prefix, wcslen(hwid_prefix)) == 0) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        /* Open the device's software registry key (Device Parameters) and read
         * the WinUSB device interface GUID(s) stored by the driver INF. */
        HKEY hKey = SetupDiOpenDevRegKey(hdi, &devInfo, DICS_FLAG_GLOBAL, 0,
                                         DIREG_DEV, KEY_READ);
        if (hKey == INVALID_HANDLE_VALUE) {
            continue;
        }

        wchar_t guid_buf[256];
        memset(guid_buf, 0, sizeof(guid_buf));
        DWORD buf_len = sizeof(guid_buf) - sizeof(wchar_t);

        /* Prefer DeviceInterfaceGUIDs (REG_MULTI_SZ, modern INFs); fall back to
         * DeviceInterfaceGUID (REG_SZ, older/zadig-generated INFs). */
        LONG reg_rc = RegQueryValueExW(hKey, L"DeviceInterfaceGUIDs", NULL, NULL,
                                       (LPBYTE)guid_buf, &buf_len);
        if (reg_rc != ERROR_SUCCESS) {
            buf_len = sizeof(guid_buf) - sizeof(wchar_t);
            RegQueryValueExW(hKey, L"DeviceInterfaceGUID", NULL, NULL,
                             (LPBYTE)guid_buf, &buf_len);
        }
        RegCloseKey(hKey);

        if (!guid_buf[0]) {
            continue;
        }

        /* Iterate GUID strings.  Both REG_SZ and REG_MULTI_SZ are covered by the
         * same NUL-terminated-string walk (REG_SZ just has one entry). */
        const wchar_t *g;
        for (g = guid_buf;
             *g && existing + added < CANDLE_MAX_DEVICES;
             g += wcslen(g) + 1)
        {
            unsigned base = existing + added;
            int n = candle_scan_guid(l, g, base);
            if (n <= 0) {
                continue;
            }

            /* Remove any entries whose path was already found by the GUID scan. */
            for (int ni = 0; ni < n; ) {
                if (candle_path_exists(l, base, l->dev[base + ni].path)) {
                    memmove(&l->dev[base + ni], &l->dev[base + ni + 1],
                            (unsigned)(n - ni - 1) * sizeof(candle_device_t));
                    n--;
                } else {
                    ni++;
                }
            }
            added += n;
        }
    }

    SetupDiDestroyDeviceInfoList(hdi);
    return added;
}

bool __stdcall candle_list_scan(candle_list_handle *list)
{
    if (list == NULL) {
        return false;
    }

    candle_list_t *l = (candle_list_t *)calloc(1, sizeof(candle_list_t));
    *list = l;
    if (l == NULL) {
        return false;
    }

    /* GUIDs for gs_usb-compatible devices on Windows.
     * candleLight / CANable / most gs_usb devices: */
    static const wchar_t *GUIDS[] = {
        L"{c15b4308-04d3-11e6-b3ea-6057189e6443}"  /* candleLight / CANable / gs_usb standard */
    };
    static const unsigned NUM_GUIDS = sizeof(GUIDS) / sizeof(GUIDS[0]);

    unsigned total = 0;
    for (unsigned g = 0; g < NUM_GUIDS; g++) {
        int n = candle_scan_guid(l, GUIDS[g], total);
        if (n < 0) {
            return false;
        }
        total += (unsigned)n;
    }

    /* VID/PID scan for devices whose device interface GUID is not in the list
     * above (e.g. CANnectivity which uses its own registered interface GUID). */
    static const struct { uint16_t vid; uint16_t pid; } VIDPIDS[] = {
        { 0x1209, 0xCA01 },  /* CANnectivity (electronut-labs) */
    };
    static const unsigned NUM_VIDPIDS = sizeof(VIDPIDS) / sizeof(VIDPIDS[0]);

    for (unsigned v = 0; v < NUM_VIDPIDS && total < CANDLE_MAX_DEVICES; v++) {
        int n = candle_scan_vidpid(l, VIDPIDS[v].vid, VIDPIDS[v].pid, total);
        if (n > 0)
            total += (unsigned)n;
    }

    l->num_devices = (uint8_t)total;
    l->last_error  = CANDLE_ERR_OK;
    return true;
}

bool __stdcall DLL candle_list_free(candle_list_handle list)
{
    free(list);
    return true;
}

bool __stdcall DLL candle_list_length(candle_list_handle list, uint8_t *len)
{
    candle_list_t *l = (candle_list_t *)list;
    *len = l->num_devices;
    return true;
}

bool __stdcall DLL candle_dev_get(candle_list_handle list, uint8_t dev_num, candle_handle *hdev)
{
    candle_list_t *l = (candle_list_t *)list;
    if (l==NULL) {
        return false;
    }

    if (dev_num >= CANDLE_MAX_DEVICES) {
        l->last_error = CANDLE_ERR_DEV_OUT_OF_RANGE;
        return false;
    }

    candle_device_t *dev = calloc(1, sizeof(candle_device_t));
    *hdev = dev;
    if (dev==NULL) {
        l->last_error = CANDLE_ERR_MALLOC;
        return false;
    }

    memcpy(dev, &l->dev[dev_num], sizeof(candle_device_t));
    l->last_error = CANDLE_ERR_OK;
    dev->last_error = CANDLE_ERR_OK;
    return true;
}


bool __stdcall DLL candle_dev_get_state(candle_handle hdev, candle_devstate_t *state)
{
    if (hdev==NULL) {
        return false;
    } else {
        candle_device_t *dev = (candle_device_t*)hdev;
        *state = dev->state;
        return true;
    }
}

wchar_t __stdcall DLL *candle_dev_get_path(candle_handle hdev)
{
    if (hdev==NULL) {
        return NULL;
    } else {
        candle_device_t *dev = (candle_device_t*)hdev;
        return dev->path;
    }
}

static bool candle_dev_interal_open(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    memset(dev->rxevents, 0, sizeof(dev->rxevents));
    memset(dev->rxurbs, 0, sizeof(dev->rxurbs));

    dev->deviceHandle = CreateFile(
        dev->path,
        GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (dev->deviceHandle == INVALID_HANDLE_VALUE) {
        dev->last_error = CANDLE_ERR_CREATE_FILE;
        return false;
    }

    if (!WinUsb_Initialize(dev->deviceHandle, &dev->winUSBHandle)) {
        dev->last_error = CANDLE_ERR_WINUSB_INITIALIZE;
        goto close_handle;
    }

    USB_INTERFACE_DESCRIPTOR ifaceDescriptor;
    if (!WinUsb_QueryInterfaceSettings(dev->winUSBHandle, 0, &ifaceDescriptor)) {
        dev->last_error = CANDLE_ERR_QUERY_INTERFACE;
        goto winusb_free;
    }

    dev->interfaceNumber = ifaceDescriptor.bInterfaceNumber;
    bool has_in = false, has_out = false;

    candle_logf(L"open path=%ls interface=%u endpoints=%u",
                dev->path,
                dev->interfaceNumber,
                ifaceDescriptor.bNumEndpoints);

    for (uint8_t i=0; i<ifaceDescriptor.bNumEndpoints; i++) {

        WINUSB_PIPE_INFORMATION pipeInfo;
        if (!WinUsb_QueryPipe(dev->winUSBHandle, 0, i, &pipeInfo)) {
            dev->last_error = CANDLE_ERR_QUERY_PIPE;
            goto winusb_free;
        }

        if (pipeInfo.PipeType == UsbdPipeTypeBulk && USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
            if (!has_in) {
                dev->bulkInPipe = pipeInfo.PipeId;
                has_in = true;
                candle_logf(L"selected bulk IN pipe=0x%02x maxPacket=%u interval=%u",
                            pipeInfo.PipeId,
                            pipeInfo.MaximumPacketSize,
                            pipeInfo.Interval);
            }
        } else if (pipeInfo.PipeType == UsbdPipeTypeBulk && USB_ENDPOINT_DIRECTION_OUT(pipeInfo.PipeId)) {
            if (!has_out) {
                dev->bulkOutPipe = pipeInfo.PipeId;
                has_out = true;
                candle_logf(L"selected bulk OUT pipe=0x%02x maxPacket=%u interval=%u",
                            pipeInfo.PipeId,
                            pipeInfo.MaximumPacketSize,
                            pipeInfo.Interval);
            }
        }

    }

    if (!has_in || !has_out) {
        dev->last_error = CANDLE_ERR_PARSE_IF_DESCR;
        goto winusb_free;
    }

    char use_raw_io = 1;
    if (!WinUsb_SetPipePolicy(dev->winUSBHandle, dev->bulkInPipe, RAW_IO, sizeof(use_raw_io), &use_raw_io)) {
        dev->last_error = CANDLE_ERR_SET_PIPE_RAW_IO;
        goto winusb_free;
    }

    if (!candle_ctrl_set_host_format(dev)) {
        goto winusb_free;
    }

    if (!candle_ctrl_get_config(dev, &dev->dconf)) {
        goto winusb_free;
    }
    candle_logf(L"device config channels=%u sw=0x%08x hw=0x%08x",
                dev->dconf.icount + 1,
                dev->dconf.sw_version,
                dev->dconf.hw_version);

        if (!candle_ctrl_get_capability(dev, 0, &dev->bt_const)) {
        dev->last_error = CANDLE_ERR_GET_BITTIMING_CONST;
        goto winusb_free;
    }
    candle_logf(L"cap ch0 feature=0x%08x fclk=%u tseg1=%u..%u tseg2=%u..%u sjw=%u brp=%u..%u inc=%u",
                dev->bt_const.feature,
                dev->bt_const.fclk_can,
                dev->bt_const.tseg1_min,
                dev->bt_const.tseg1_max,
                dev->bt_const.tseg2_min,
                dev->bt_const.tseg2_max,
                dev->bt_const.sjw_max,
                dev->bt_const.brp_min,
                dev->bt_const.brp_max,
                dev->bt_const.brp_inc);

    /* Query capabilities for each channel on multi-channel devices */
    uint8_t num_channels = dev->dconf.icount + 1;
    if (num_channels > 8) num_channels = 8;
    for (uint8_t ch = 0; ch < num_channels; ch++) {
        if (!candle_ctrl_get_capability(dev, ch, &dev->ch_caps[ch])) {
            /* Fall back to channel 0 capabilities for this channel */
            memcpy(&dev->ch_caps[ch], &dev->bt_const, sizeof(candle_capability_t));
            candle_logf(L"cap ch%u failed, falling back to ch0", ch);
        } else {
            candle_logf(L"cap ch%u feature=0x%08x fclk=%u",
                        ch,
                        dev->ch_caps[ch].feature,
                        dev->ch_caps[ch].fclk_can);
        }
    }

    /* Pre-allocate a manual-reset event for timed overlapped writes.  Reusing
     * one event per device (writes are serialised by writeMutex) avoids
     * per-frame CreateEvent overhead at high CAN frame rates. */
    dev->txEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!dev->txEvent) {
        dev->last_error = CANDLE_ERR_MALLOC;
        goto winusb_free;
    }

    dev->last_error = CANDLE_ERR_OK;
    return true;

winusb_free:
    WinUsb_Free(dev->winUSBHandle);
    dev->winUSBHandle = NULL;

close_handle:
    CloseHandle(dev->deviceHandle);
    dev->deviceHandle = NULL;
    return false;

}

static bool candle_prepare_read(candle_device_t *dev, unsigned urb_num)
{
    if (dev->rxurbs[urb_num].pending) {
        dev->last_error = CANDLE_ERR_PREPARE_READ;
        return false;
    }

    if (dev->rxurbs[urb_num].ovl.hEvent == NULL) {
        dev->last_error = CANDLE_ERR_PREPARE_READ;
        return false;
    }

    ResetEvent(dev->rxurbs[urb_num].ovl.hEvent);

    BOOL rc = WinUsb_ReadPipe(
        dev->winUSBHandle,
        dev->bulkInPipe,
        dev->rxurbs[urb_num].buf,
        sizeof(dev->rxurbs[urb_num].buf),
        NULL,
        &dev->rxurbs[urb_num].ovl
    );

    if (rc) {
        /* Synchronous completion: data is already in buf and the event is
         * signaled. WaitForMultipleObjects will return immediately on the
         * next call and GetOverlappedResult will succeed, so this is fine. */
        dev->rxurbs[urb_num].pending = true;
        dev->last_error = CANDLE_ERR_OK;
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        dev->rxurbs[urb_num].pending = true;
        dev->last_error = CANDLE_ERR_OK;
        return true;
    }

    candle_logf(L"prepare read urb=%u failed winerr=%lu", urb_num, err);
    dev->last_error = CANDLE_ERR_PREPARE_READ;
    return false;
}

static bool candle_close_rxurbs(candle_device_t *dev)
{
    if (dev->winUSBHandle != NULL) {
        WinUsb_AbortPipe(dev->winUSBHandle, dev->bulkInPipe);
    }

    for (unsigned i=0; i<CANDLE_URB_COUNT; i++) {
        if (dev->rxurbs[i].pending) {
            CancelIoEx(dev->deviceHandle, &dev->rxurbs[i].ovl);

            DWORD bytes_transfered;
            WinUsb_GetOverlappedResult(dev->winUSBHandle,
                                       &dev->rxurbs[i].ovl,
                                       &bytes_transfered,
                                       TRUE);
            dev->rxurbs[i].pending = false;
        }

        if (dev->rxevents[i] != NULL) {
            CloseHandle(dev->rxevents[i]);
            dev->rxevents[i] = NULL;
            memset(&dev->rxurbs[i].ovl, 0, sizeof(dev->rxurbs[i].ovl));
        }
    }
    return true;
}

static void candle_release_open_handles(candle_device_t *dev)
{
    candle_close_rxurbs(dev);

    if (dev->txEvent) {
        CloseHandle(dev->txEvent);
        dev->txEvent = NULL;
    }

    if (dev->winUSBHandle) {
        WinUsb_Free(dev->winUSBHandle);
        dev->winUSBHandle = NULL;
    }

    if (dev->deviceHandle && dev->deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->deviceHandle);
        dev->deviceHandle = NULL;
    }
}


bool __stdcall DLL candle_dev_open(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    if (candle_dev_interal_open(dev)) {
        for (unsigned i=0; i<CANDLE_URB_COUNT; i++) {
            HANDLE ev = CreateEvent(NULL, true, false, NULL);
            if (ev == NULL) {
                dev->last_error = CANDLE_ERR_MALLOC;
                candle_err_t last_error = dev->last_error;
                candle_release_open_handles(dev);
                dev->last_error = last_error;
                return false;
            }
            dev->rxevents[i] = ev;
            dev->rxurbs[i].ovl.hEvent = ev;
            if (!candle_prepare_read(dev, i)) {
                candle_err_t last_error = dev->last_error;
                candle_release_open_handles(dev);
                dev->last_error = last_error;
                return false; // keep last_error from prepare_read call
            }
        }
        dev->last_error = CANDLE_ERR_OK;
        return true;
    } else {
        return false; // keep last_error from open_device call
    }

}

bool __stdcall DLL candle_dev_get_timestamp_us(candle_handle hdev, uint32_t *timestamp_us)
{
	return candle_ctrl_get_timestamp(hdev, timestamp_us);
}

bool __stdcall DLL candle_dev_close(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    candle_release_open_handles(dev);

    dev->last_error = CANDLE_ERR_OK;
    return true;
}

bool __stdcall DLL candle_dev_free(candle_handle hdev)
{
    free(hdev);
    return true;
}

candle_err_t __stdcall DLL candle_dev_last_error(candle_handle hdev)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return dev->last_error;
}

bool __stdcall DLL candle_channel_count(candle_handle hdev, uint8_t *num_channels)
{
    // TODO check if info was already read from device; try to do so; throw error...
    candle_device_t *dev = (candle_device_t*)hdev;
    *num_channels = dev->dconf.icount+1;
    return true;
}

bool __stdcall DLL candle_channel_get_capabilities(candle_handle hdev, uint8_t ch, candle_capability_t *cap)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    uint8_t num_channels = dev->dconf.icount + 1;
    if (ch < num_channels && ch < 8) {
        memcpy(cap, &dev->ch_caps[ch], sizeof(candle_capability_t));
    } else {
        memcpy(cap, &dev->bt_const, sizeof(candle_capability_t));
    }
    return true;
}

bool __stdcall DLL candle_channel_get_state(candle_handle hdev, uint8_t ch, candle_can_state_t *state)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_device_state_t ds;
    if (!candle_ctrl_get_state(dev, ch, &ds)) {
        return false;
    }
    *state = (candle_can_state_t)ds.state;
    return true;
}

bool __stdcall DLL candle_channel_set_tdc_auto(candle_handle hdev, uint8_t ch)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_device_tdc_t tdc = { 0, 0, 0, 1u << 1 }; /* GS_CAN_TDC_MODE_AUTO: firmware computes TDCO */
    return candle_ctrl_set_tdc(dev, ch, &tdc);
}

bool __stdcall DLL candle_channel_bus_off_recovery(candle_handle hdev, uint8_t ch)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_bus_off_recovery(dev, ch);
}

bool __stdcall DLL candle_channel_set_timing(candle_handle hdev, uint8_t ch, candle_bittiming_t *data)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_bittiming(dev, ch, data);
}

bool __stdcall DLL candle_channel_set_bitrate(candle_handle hdev, uint8_t ch, uint32_t bitrate)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;

    if (dev->bt_const.fclk_can != 48000000) {
        /* this function only works for the candleLight base clock of 48MHz */
        dev->last_error = CANDLE_ERR_BITRATE_FCLK;
        return false;
    }

    candle_bittiming_t t;
    t.prop_seg = 1;
    t.sjw = 1;
    t.phase_seg1 = 13 - t.prop_seg;
    t.phase_seg2 = 2;

    switch (bitrate) {
        case 10000:
            t.brp = 300;
            break;

        case 20000:
            t.brp = 150;
            break;

        case 50000:
            t.brp = 60;
            break;

        case 83333:
            t.brp = 36;
            break;

        case 100000:
            t.brp = 30;
            break;

        case 125000:
            t.brp = 24;
            break;

        case 250000:
            t.brp = 12;
            break;

        case 500000:
            t.brp = 6;
            break;

        case 800000:
            t.brp = 4;
            t.phase_seg1 = 12 - t.prop_seg;
            t.phase_seg2 = 2;
            break;

        case 1000000:
            t.brp = 3;
            break;

        default:
            dev->last_error = CANDLE_ERR_BITRATE_UNSUPPORTED;
            return false;
    }

    return candle_ctrl_set_bittiming(dev, ch, &t);
}

bool __stdcall DLL candle_channel_start(candle_handle hdev, uint8_t ch, uint32_t flags)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    candle_capability_t *cap = (ch < 8) ? &dev->ch_caps[ch] : &dev->bt_const;

    if (cap->feature & CANDLE_FEATURE_HW_TIMESTAMP) {
        flags |= CANDLE_MODE_HW_TIMESTAMP;
    } else {
        candle_logf(L"channel %u has no HW timestamp capability; starting without timestamp flag", ch);
    }

    bool rc = candle_ctrl_set_device_mode(dev, ch, CANDLE_DEVMODE_START, flags);
    candle_logf(L"channel %u start flags=0x%08x result=%u err=%u",
                ch,
                flags,
                rc ? 1 : 0,
                dev->last_error);
    return rc;
}

bool __stdcall DLL candle_channel_stop(candle_handle hdev, uint8_t ch)
{
    // TODO ensure device is open, check channel count..
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_device_mode(dev, ch, CANDLE_DEVMODE_RESET, 0);
}

/* Write len bytes from buf to the OUT pipe, aborting after 300 ms.
 * Writes are serialised by writeMutex in CandleApiInterface so dev->txEvent
 * is never accessed by two threads simultaneously. */
static bool candle_write_pipe_timed(candle_device_t *dev, uint8_t *buf, DWORD len)
{
    OVERLAPPED ovl;
    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = dev->txEvent;
    ResetEvent(dev->txEvent);

    BOOL rc = WinUsb_WritePipe(dev->winUSBHandle, dev->bulkOutPipe,
                               buf, len, NULL, &ovl);
    if (rc) {
        return true;   /* completed synchronously */
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return false;  /* hard error */
    }

    if (WaitForSingleObject(dev->txEvent, 150) != WAIT_OBJECT_0) {
        /* Timed out: cancel the transfer and restore the pipe to a clean state. */
        WinUsb_AbortPipe(dev->winUSBHandle, dev->bulkOutPipe);
        DWORD dummy = 0;
        WinUsb_GetOverlappedResult(dev->winUSBHandle, &ovl, &dummy, TRUE);
        WinUsb_ResetPipe(dev->winUSBHandle, dev->bulkOutPipe);
        return false;
    }

    DWORD transferred = 0;
    return WinUsb_GetOverlappedResult(dev->winUSBHandle, &ovl, &transferred, FALSE) != FALSE;
}

bool __stdcall DLL candle_frame_send(candle_handle hdev, uint8_t ch, candle_frame_t *frame)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    frame->echo_id = 0;
    frame->channel = ch;
    bool rc = candle_write_pipe_timed(dev, (uint8_t*)frame, sizeof(*frame));
    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SEND_FRAME;
    return rc;
}

bool __stdcall DLL candle_frame_read(candle_handle hdev, candle_frame_t *frame, uint32_t timeout_ms)
{
    // TODO ensure device is open..
    candle_device_t *dev = (candle_device_t*)hdev;

    DWORD wait_result = WaitForMultipleObjects(CANDLE_URB_COUNT, dev->rxevents, false, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        dev->last_error = CANDLE_ERR_READ_TIMEOUT;
        return false;
    }

    if ( (wait_result < WAIT_OBJECT_0) || (wait_result >= WAIT_OBJECT_0 + CANDLE_URB_COUNT) ) {
        dev->last_error = CANDLE_ERR_READ_WAIT;
        return false;
    }

    DWORD urb_num = wait_result - WAIT_OBJECT_0;
    DWORD bytes_transfered;

    if (!WinUsb_GetOverlappedResult(dev->winUSBHandle, &dev->rxurbs[urb_num].ovl, &bytes_transfered, false)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            ResetEvent(dev->rxurbs[urb_num].ovl.hEvent);
        } else {
            dev->rxurbs[urb_num].pending = false;
            candle_prepare_read(dev, urb_num);
        }
        candle_logf(L"classic read result failed urb=%u winerr=%lu", urb_num, err);
        dev->last_error = CANDLE_ERR_READ_RESULT;
        return false;
    }
    dev->rxurbs[urb_num].pending = false;

    if (bytes_transfered < sizeof(*frame)-4) {
        candle_prepare_read(dev, urb_num);
        candle_logf(L"classic read too small urb=%u bytes=%lu min=%u",
                    urb_num,
                    bytes_transfered,
                    (unsigned)(sizeof(*frame) - 4));
        dev->last_error = CANDLE_ERR_READ_SIZE;
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    DWORD copy_len = (bytes_transfered < sizeof(*frame)) ? bytes_transfered : sizeof(*frame);
    memcpy(frame, dev->rxurbs[urb_num].buf, copy_len);
    candle_logf_verbose(L"classic read urb=%u bytes=%lu echo=0x%08x can_id=0x%08x dlc=%u ch=%u flags=0x%02x ts=%u",
                urb_num,
                bytes_transfered,
                frame->echo_id,
                frame->can_id,
                frame->can_dlc,
                frame->channel,
                frame->flags,
                frame->timestamp_us);

    return candle_prepare_read(dev, urb_num);
}

candle_frametype_t __stdcall DLL candle_frame_type(candle_frame_t *frame)
{
    if (frame->echo_id != 0xFFFFFFFF) {
        return CANDLE_FRAMETYPE_ECHO;
    };

    if (frame->can_id & CANDLE_ID_ERR) {
        return CANDLE_FRAMETYPE_ERROR;
    }

    return CANDLE_FRAMETYPE_RECEIVE;
}

uint32_t __stdcall DLL candle_frame_id(candle_frame_t *frame)
{
    return frame->can_id & 0x1FFFFFFF;
}

bool __stdcall DLL candle_frame_is_extended_id(candle_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_EXTENDED) != 0;
}

bool __stdcall DLL candle_frame_is_rtr(candle_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_RTR) != 0;
}

uint8_t __stdcall DLL candle_frame_dlc(candle_frame_t *frame)
{
    return frame->can_dlc;
}

uint8_t __stdcall DLL *candle_frame_data(candle_frame_t *frame)
{
    return frame->data;
}

uint32_t __stdcall DLL candle_frame_timestamp_us(candle_frame_t *frame)
{
    return frame->timestamp_us;
}

/* ---- CAN FD extensions ---- */

bool __stdcall DLL candle_channel_set_data_timing(candle_handle hdev, uint8_t ch, candle_bittiming_t *data)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    return candle_ctrl_set_data_bittiming(dev, ch, data);
}

bool __stdcall DLL candle_fd_frame_send(candle_handle hdev, uint8_t ch, candle_fd_frame_t *frame)
{
    candle_device_t *dev = (candle_device_t*)hdev;
    frame->echo_id = 0;
    frame->channel = ch;
    bool rc = candle_write_pipe_timed(dev, (uint8_t*)frame, sizeof(*frame));
    dev->last_error = rc ? CANDLE_ERR_OK : CANDLE_ERR_SEND_FRAME;
    return rc;
}

bool __stdcall DLL candle_fd_frame_read(candle_handle hdev, candle_fd_frame_t *frame, uint32_t timeout_ms)
{
    candle_device_t *dev = (candle_device_t*)hdev;

    DWORD wait_result = WaitForMultipleObjects(CANDLE_URB_COUNT, dev->rxevents, false, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        dev->last_error = CANDLE_ERR_READ_TIMEOUT;
        return false;
    }

    if ( (wait_result < WAIT_OBJECT_0) || (wait_result >= WAIT_OBJECT_0 + CANDLE_URB_COUNT) ) {
        dev->last_error = CANDLE_ERR_READ_WAIT;
        return false;
    }

    DWORD urb_num = wait_result - WAIT_OBJECT_0;
    DWORD bytes_transfered;

    if (!WinUsb_GetOverlappedResult(dev->winUSBHandle, &dev->rxurbs[urb_num].ovl, &bytes_transfered, false)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            ResetEvent(dev->rxurbs[urb_num].ovl.hEvent);
        } else {
            dev->rxurbs[urb_num].pending = false;
            candle_prepare_read(dev, urb_num);
        }
        candle_logf(L"fd read result failed urb=%u winerr=%lu", urb_num, err);
        dev->last_error = CANDLE_ERR_READ_RESULT;
        return false;
    }
    dev->rxurbs[urb_num].pending = false;

    /* Minimum: classic CAN header (12 bytes) + at least 8 data bytes = 20 bytes */
    static const DWORD classic_min = sizeof(candle_frame_t) - 4;
    if (bytes_transfered < classic_min) {
        candle_prepare_read(dev, urb_num);
        candle_logf(L"fd read too small urb=%u bytes=%lu min=%lu",
                    urb_num,
                    bytes_transfered,
                    classic_min);
        dev->last_error = CANDLE_ERR_READ_SIZE;
        return false;
    }

    memset(frame, 0, sizeof(*frame));

    /*
     * Detect frame type from the flags byte (offset 10 in both structs).
     * Classic CAN frames: header(12) + data(8) + timestamp(4) = 24 bytes total.
     *
     * FD frames come in two wire formats:
     *  - Legacy fixed (candleLight/CANable 1.x): always 80 bytes — header(12) +
     *    data[64] + timestamp(4).  The timestamp is ALWAYS at offset 76, regardless
     *    of the actual DLC.  Identified by bytes_transferred == sizeof(candle_fd_frame_t).
     *  - Variable-length (CANnectivity/Zephyr): header(12) + actual_data(DLC) +
     *    timestamp(4).  Identified by bytes_transferred < sizeof(candle_fd_frame_t).
     */
    bool is_fd_frame = (dev->rxurbs[urb_num].buf[10] & CANDLE_FRAME_FLAG_FD) != 0;

    if (is_fd_frame) {
        /* can_dlc is at byte offset 8 in both classic and FD wire frames. */
        const uint8_t raw_dlc = dev->rxurbs[urb_num].buf[8];
        const DWORD data_len  = candle_dlc_to_len(raw_dlc);
        const DWORD min_size  = 12 + data_len; /* header + data, without timestamp */

        if (bytes_transfered < min_size) {
            candle_prepare_read(dev, urb_num);
            candle_logf(L"fd read FD frame too small urb=%u bytes=%lu min=%lu flags=0x%02x dlc=%u",
                        urb_num,
                        bytes_transfered,
                        min_size,
                        dev->rxurbs[urb_num].buf[10],
                        raw_dlc);
            dev->last_error = CANDLE_ERR_READ_SIZE;
            return false;
        }

        /* Copy the fixed 12-byte header (echo_id … reserved). */
        memcpy(frame, dev->rxurbs[urb_num].buf, 12);
        /* Copy data at offset 12 into the struct's data field. */
        memcpy(frame->data, dev->rxurbs[urb_num].buf + 12, data_len);

        /* Timestamp location depends on the wire format (see comment above). */
        const DWORD fixed_ts_offset = (DWORD)(sizeof(candle_fd_frame_t) - sizeof(uint32_t)); /* = 76 */
        const DWORD ts_offset = (bytes_transfered >= (DWORD)sizeof(candle_fd_frame_t))
                                ? fixed_ts_offset
                                : min_size;
        if (bytes_transfered >= ts_offset + (DWORD)sizeof(uint32_t)) {
            memcpy(&frame->timestamp_us, dev->rxurbs[urb_num].buf + ts_offset, sizeof(uint32_t));
        }
        /* else: timestamp stays zero from memset above */
    } else {
        /* Classic CAN frame — copy into FD struct, fixing the timestamp position */
        candle_frame_t classic;
        DWORD copy_len = (bytes_transfered < sizeof(classic)) ? bytes_transfered : sizeof(classic);
        memcpy(&classic, dev->rxurbs[urb_num].buf, copy_len);

        frame->echo_id      = classic.echo_id;
        frame->can_id       = classic.can_id;
        frame->can_dlc      = classic.can_dlc;
        frame->channel      = classic.channel;
        frame->flags        = classic.flags;
        frame->reserved     = classic.reserved;
        memcpy(frame->data, classic.data, 8);
        frame->timestamp_us = (bytes_transfered >= sizeof(classic)) ? classic.timestamp_us : 0;
    }
    candle_logf_verbose(L"fd read urb=%u bytes=%lu is_fd=%u echo=0x%08x can_id=0x%08x dlc=%u ch=%u flags=0x%02x ts=%u",
                urb_num,
                bytes_transfered,
                is_fd_frame ? 1 : 0,
                frame->echo_id,
                frame->can_id,
                frame->can_dlc,
                frame->channel,
                frame->flags,
                frame->timestamp_us);

    return candle_prepare_read(dev, urb_num);
}

candle_frametype_t __stdcall DLL candle_fd_frame_type(candle_fd_frame_t *frame)
{
    if (frame->echo_id != 0xFFFFFFFF) {
        return CANDLE_FRAMETYPE_ECHO;
    }
    if (frame->can_id & CANDLE_ID_ERR) {
        return CANDLE_FRAMETYPE_ERROR;
    }
    return CANDLE_FRAMETYPE_RECEIVE;
}

uint32_t __stdcall DLL candle_fd_frame_id(candle_fd_frame_t *frame)
{
    return frame->can_id & 0x1FFFFFFF;
}

bool __stdcall DLL candle_fd_frame_is_extended_id(candle_fd_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_EXTENDED) != 0;
}

bool __stdcall DLL candle_fd_frame_is_rtr(candle_fd_frame_t *frame)
{
    return (frame->can_id & CANDLE_ID_RTR) != 0;
}

bool __stdcall DLL candle_fd_frame_is_fd(candle_fd_frame_t *frame)
{
    return (frame->flags & CANDLE_FRAME_FLAG_FD) != 0;
}

bool __stdcall DLL candle_fd_frame_is_brs(candle_fd_frame_t *frame)
{
    return (frame->flags & CANDLE_FRAME_FLAG_BRS) != 0;
}

uint8_t __stdcall DLL candle_fd_frame_dlc(candle_fd_frame_t *frame)
{
    return frame->can_dlc;
}

uint8_t __stdcall DLL *candle_fd_frame_data(candle_fd_frame_t *frame)
{
    return frame->data;
}

uint32_t __stdcall DLL candle_fd_frame_timestamp_us(candle_fd_frame_t *frame)
{
    return frame->timestamp_us;
}
