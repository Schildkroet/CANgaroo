/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
  Copyright (c) 2026 Schildkroet

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

// candle_usb.h on Windows: SetupAPI enumeration and WinUSB transfers. The
// gs_usb protocol itself is in candle.c.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <winbase.h>
#include <winusb.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#undef __CRT__NO_INLINE
#include <strsafe.h>
#define __CRT__NO_INLINE

#include <stdlib.h>

#include "candle_usb.h"

#define CANDLE_URB_COUNT 30

typedef struct {
    OVERLAPPED ovl;
    bool pending;
    uint8_t buf[512];
} candle_rx_urb;

struct candle_usb {
    HANDLE deviceHandle;
    WINUSB_INTERFACE_HANDLE winUSBHandle;
    UCHAR interfaceNumber;
    UCHAR bulkInPipe;
    UCHAR bulkOutPipe;
    HANDLE txEvent;     /* pre-allocated event for timed overlapped writes */
    bool reading;
    candle_rx_urb rxurbs[CANDLE_URB_COUNT];
    HANDLE rxevents[CANDLE_URB_COUNT];
};


// ── Enumeration ──────────────────────────────────────────────────────────────

/* Return true when path already appears in paths[0..count-1]. */
static bool candle_path_exists(wchar_t paths[][CANDLE_USB_PATH_LEN], int count, const wchar_t *path)
{
    for (int i = 0; i < count; i++) {
        if (wcscmp(paths[i], path) == 0)
            return true;
    }
    return false;
}

static bool candle_read_path(HDEVINFO hdi, SP_DEVICE_INTERFACE_DATA interfaceData, wchar_t *path, candle_err_t *err)
{
    /* get required length first (this call always fails with an error) */
    ULONG requiredLength=0;
    SetupDiGetDeviceInterfaceDetail(hdi, &interfaceData, NULL, 0, &requiredLength, NULL);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        *err = CANDLE_ERR_SETUPDI_IF_DETAILS;
        return false;
    }

    PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA) LocalAlloc(LMEM_FIXED, requiredLength);

    if (detail_data != NULL) {
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    } else {
        *err = CANDLE_ERR_MALLOC;
        return false;
    }

    bool retval = true;
    ULONG length = requiredLength;
    if (!SetupDiGetDeviceInterfaceDetail(hdi, &interfaceData, detail_data, length, &requiredLength, NULL) ) {
        *err = CANDLE_ERR_SETUPDI_IF_DETAILS2;
        retval = false;
    } else if (FAILED(StringCchCopy(path, CANDLE_USB_PATH_LEN, detail_data->DevicePath))) {
        *err = CANDLE_ERR_PATH_LEN;
        retval = false;
    }

    LocalFree(detail_data);
    return retval;
}

/* Scan one GUID and append found paths starting at offset.
 * Returns the number of paths appended, or -1 on a hard error (*err set). */
static int candle_scan_guid(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, const wchar_t *guid_str,
                            int offset, candle_err_t *err)
{
    GUID guid;
    if (CLSIDFromString(guid_str, &guid) != NOERROR) {
        *err = CANDLE_ERR_CLSID;
        return -1;
    }

    HDEVINFO hdi = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) {
        /* No devices with this GUID present — not a hard error. */
        return 0;
    }

    int found = 0;
    for (unsigned i = 0; (offset + (int)i) < max; i++) {
        SP_DEVICE_INTERFACE_DATA interfaceData;
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        if (!SetupDiEnumDeviceInterfaces(hdi, NULL, &guid, i, &interfaceData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                *err = CANDLE_ERR_SETUPDI_IF_ENUM;
                found = -1;
            }
            break;
        }

        if (!candle_read_path(hdi, interfaceData, paths[offset + (int)i], err)) {
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
 * only those devices that are not already present in paths[0..existing-1].
 * Returns the number of new devices added. */
static int candle_scan_vidpid(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, uint16_t vid, uint16_t pid,
                              int existing, candle_err_t *err)
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
         SetupDiEnumDeviceInfo(hdi, i, &devInfo) && existing + added < max;
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
             *g && existing + added < max;
             g += wcslen(g) + 1)
        {
            int base = existing + added;
            int n = candle_scan_guid(paths, max, g, base, err);
            if (n <= 0) {
                continue;
            }

            /* Remove any entries whose path was already found by the GUID scan. */
            for (int ni = 0; ni < n; ) {
                if (candle_path_exists(paths, base, paths[base + ni])) {
                    memmove(&paths[base + ni], &paths[base + ni + 1],
                            (unsigned)(n - ni - 1) * sizeof(paths[0]));
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

int candle_usb_scan(wchar_t paths[][CANDLE_USB_PATH_LEN], int max, candle_err_t *err)
{
    /* GUIDs for gs_usb-compatible devices on Windows.
     * candleLight / CANable / most gs_usb devices: */
    static const wchar_t *GUIDS[] = {
        L"{c15b4308-04d3-11e6-b3ea-6057189e6443}"  /* candleLight / CANable / gs_usb standard */
    };
    static const unsigned NUM_GUIDS = sizeof(GUIDS) / sizeof(GUIDS[0]);

    int total = 0;
    for (unsigned g = 0; g < NUM_GUIDS; g++) {
        int n = candle_scan_guid(paths, max, GUIDS[g], total, err);
        if (n < 0) {
            return -1;
        }
        total += n;
    }

    /* VID/PID scan for devices whose device interface GUID is not in the list
     * above (e.g. CANnectivity which uses its own registered interface GUID). */
    static const struct { uint16_t vid; uint16_t pid; } VIDPIDS[] = {
        { 0x1209, 0xCA01 },  /* CANnectivity (electronut-labs) */
    };
    static const unsigned NUM_VIDPIDS = sizeof(VIDPIDS) / sizeof(VIDPIDS[0]);

    for (unsigned v = 0; v < NUM_VIDPIDS && total < max; v++) {
        int n = candle_scan_vidpid(paths, max, VIDPIDS[v].vid, VIDPIDS[v].pid, total, err);
        if (n > 0)
            total += n;
    }

    *err = CANDLE_ERR_OK;
    return total;
}


// ── Open / close ─────────────────────────────────────────────────────────────

static bool candle_prepare_read(candle_usb_t *usb, unsigned urb_num)
{
    if (usb->rxurbs[urb_num].pending) {
        return false;
    }

    if (usb->rxurbs[urb_num].ovl.hEvent == NULL) {
        return false;
    }

    ResetEvent(usb->rxurbs[urb_num].ovl.hEvent);

    BOOL rc = WinUsb_ReadPipe(
        usb->winUSBHandle,
        usb->bulkInPipe,
        usb->rxurbs[urb_num].buf,
        sizeof(usb->rxurbs[urb_num].buf),
        NULL,
        &usb->rxurbs[urb_num].ovl
    );

    if (rc) {
        /* Synchronous completion: data is already in buf and the event is
         * signaled. WaitForMultipleObjects will return immediately on the
         * next call and GetOverlappedResult will succeed, so this is fine. */
        usb->rxurbs[urb_num].pending = true;
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        usb->rxurbs[urb_num].pending = true;
        return true;
    }

    candle_logf(L"prepare read urb=%u failed winerr=%lu", urb_num, err);
    return false;
}

static void candle_close_rxurbs(candle_usb_t *usb)
{
    if (usb->winUSBHandle != NULL) {
        WinUsb_AbortPipe(usb->winUSBHandle, usb->bulkInPipe);
    }

    for (unsigned i=0; i<CANDLE_URB_COUNT; i++) {
        if (usb->rxurbs[i].pending) {
            CancelIoEx(usb->deviceHandle, &usb->rxurbs[i].ovl);

            DWORD bytes_transfered;
            WinUsb_GetOverlappedResult(usb->winUSBHandle,
                                       &usb->rxurbs[i].ovl,
                                       &bytes_transfered,
                                       TRUE);
            usb->rxurbs[i].pending = false;
        }

        if (usb->rxevents[i] != NULL) {
            CloseHandle(usb->rxevents[i]);
            usb->rxevents[i] = NULL;
            memset(&usb->rxurbs[i].ovl, 0, sizeof(usb->rxurbs[i].ovl));
        }
    }
}

void candle_usb_close(candle_usb_t *usb)
{
    if (usb == NULL) {
        return;
    }

    candle_close_rxurbs(usb);

    if (usb->txEvent) {
        CloseHandle(usb->txEvent);
        usb->txEvent = NULL;
    }

    if (usb->winUSBHandle) {
        WinUsb_Free(usb->winUSBHandle);
        usb->winUSBHandle = NULL;
    }

    if (usb->deviceHandle && usb->deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(usb->deviceHandle);
        usb->deviceHandle = NULL;
    }

    free(usb);
}

candle_usb_t *candle_usb_open(const wchar_t *path, bool start_reads, candle_err_t *err)
{
    candle_usb_t *usb = (candle_usb_t *)calloc(1, sizeof(candle_usb_t));
    if (usb == NULL) {
        *err = CANDLE_ERR_MALLOC;
        return NULL;
    }

    usb->deviceHandle = CreateFile(
        path,
        GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (usb->deviceHandle == INVALID_HANDLE_VALUE) {
        usb->deviceHandle = NULL;
        *err = CANDLE_ERR_CREATE_FILE;
        goto fail;
    }

    if (!WinUsb_Initialize(usb->deviceHandle, &usb->winUSBHandle)) {
        usb->winUSBHandle = NULL;
        *err = CANDLE_ERR_WINUSB_INITIALIZE;
        goto fail;
    }

    USB_INTERFACE_DESCRIPTOR ifaceDescriptor;
    if (!WinUsb_QueryInterfaceSettings(usb->winUSBHandle, 0, &ifaceDescriptor)) {
        *err = CANDLE_ERR_QUERY_INTERFACE;
        goto fail;
    }

    usb->interfaceNumber = ifaceDescriptor.bInterfaceNumber;
    bool has_in = false, has_out = false;

    candle_logf(L"open path=%ls interface=%u endpoints=%u",
                path,
                usb->interfaceNumber,
                ifaceDescriptor.bNumEndpoints);

    for (uint8_t i=0; i<ifaceDescriptor.bNumEndpoints; i++) {

        WINUSB_PIPE_INFORMATION pipeInfo;
        if (!WinUsb_QueryPipe(usb->winUSBHandle, 0, i, &pipeInfo)) {
            *err = CANDLE_ERR_QUERY_PIPE;
            goto fail;
        }

        if (pipeInfo.PipeType == UsbdPipeTypeBulk && USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
            if (!has_in) {
                usb->bulkInPipe = pipeInfo.PipeId;
                has_in = true;
                candle_logf(L"selected bulk IN pipe=0x%02x maxPacket=%u interval=%u",
                            pipeInfo.PipeId,
                            pipeInfo.MaximumPacketSize,
                            pipeInfo.Interval);
            }
        } else if (pipeInfo.PipeType == UsbdPipeTypeBulk && USB_ENDPOINT_DIRECTION_OUT(pipeInfo.PipeId)) {
            if (!has_out) {
                usb->bulkOutPipe = pipeInfo.PipeId;
                has_out = true;
                candle_logf(L"selected bulk OUT pipe=0x%02x maxPacket=%u interval=%u",
                            pipeInfo.PipeId,
                            pipeInfo.MaximumPacketSize,
                            pipeInfo.Interval);
            }
        }

    }

    if (!has_in || !has_out) {
        *err = CANDLE_ERR_PARSE_IF_DESCR;
        goto fail;
    }

    char use_raw_io = 1;
    if (!WinUsb_SetPipePolicy(usb->winUSBHandle, usb->bulkInPipe, RAW_IO, sizeof(use_raw_io), &use_raw_io)) {
        *err = CANDLE_ERR_SET_PIPE_RAW_IO;
        goto fail;
    }

    /* Pre-allocate a manual-reset event for timed overlapped writes.  Reusing
     * one event per device (writes are serialised by writeMutex) avoids
     * per-frame CreateEvent overhead at high CAN frame rates. */
    usb->txEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!usb->txEvent) {
        *err = CANDLE_ERR_MALLOC;
        goto fail;
    }

    if (start_reads) {
        for (unsigned i=0; i<CANDLE_URB_COUNT; i++) {
            HANDLE ev = CreateEvent(NULL, true, false, NULL);
            if (ev == NULL) {
                *err = CANDLE_ERR_MALLOC;
                goto fail;
            }
            usb->rxevents[i] = ev;
            usb->rxurbs[i].ovl.hEvent = ev;
            if (!candle_prepare_read(usb, i)) {
                *err = CANDLE_ERR_PREPARE_READ;
                goto fail;
            }
        }
        usb->reading = true;
    }

    *err = CANDLE_ERR_OK;
    return usb;

fail:
    candle_usb_close(usb);
    return NULL;
}

uint8_t candle_usb_interface_number(const candle_usb_t *usb)
{
    return usb->interfaceNumber;
}


// ── Transfers ────────────────────────────────────────────────────────────────

bool candle_usb_control(candle_usb_t *usb, uint8_t request, uint8_t requesttype,
                        uint16_t value, uint16_t index, void *data, uint16_t size)
{
    WINUSB_SETUP_PACKET packet;
    memset(&packet, 0, sizeof(packet));
    packet.Request = request;
    packet.RequestType = requesttype;
    packet.Value = value;
    packet.Index = index;
    packet.Length = size;

    unsigned long bytes_sent = 0;
    BOOL rc = WinUsb_ControlTransfer(usb->winUSBHandle, packet, (uint8_t*)data, size, &bytes_sent, 0);
    if (!rc) {
        candle_logf_verbose(L"ctrl req=0x%02x failed winerr=%lu", request, GetLastError());
    }
    return rc != FALSE;
}

/* Write len bytes from buf to the OUT pipe, aborting after timeout_ms.
 * Writes are serialised by writeMutex in CandleApiInterface so usb->txEvent
 * is never accessed by two threads simultaneously. */
bool candle_usb_write(candle_usb_t *usb, const void *data, uint32_t len, uint32_t timeout_ms)
{
    OVERLAPPED ovl;
    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = usb->txEvent;
    ResetEvent(usb->txEvent);

    BOOL rc = WinUsb_WritePipe(usb->winUSBHandle, usb->bulkOutPipe,
                               (PUCHAR)data, len, NULL, &ovl);
    if (rc) {
        return true;   /* completed synchronously */
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return false;  /* hard error */
    }

    if (WaitForSingleObject(usb->txEvent, timeout_ms) != WAIT_OBJECT_0) {
        /* Timed out: cancel the transfer and restore the pipe to a clean state. */
        WinUsb_AbortPipe(usb->winUSBHandle, usb->bulkOutPipe);
        DWORD dummy = 0;
        WinUsb_GetOverlappedResult(usb->winUSBHandle, &ovl, &dummy, TRUE);
        WinUsb_ResetPipe(usb->winUSBHandle, usb->bulkOutPipe);
        return false;
    }

    DWORD transferred = 0;
    return WinUsb_GetOverlappedResult(usb->winUSBHandle, &ovl, &transferred, FALSE) != FALSE;
}

candle_err_t candle_usb_read(candle_usb_t *usb, uint8_t *buf, uint32_t bufsize,
                             uint32_t *len, uint32_t timeout_ms)
{
    if (!usb->reading) {
        return CANDLE_ERR_READ_WAIT;
    }

    DWORD wait_result = WaitForMultipleObjects(CANDLE_URB_COUNT, usb->rxevents, false, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        return CANDLE_ERR_READ_TIMEOUT;
    }

    if (wait_result >= WAIT_OBJECT_0 + CANDLE_URB_COUNT) {
        return CANDLE_ERR_READ_WAIT;
    }

    DWORD urb_num = wait_result - WAIT_OBJECT_0;
    DWORD bytes_transfered;

    if (!WinUsb_GetOverlappedResult(usb->winUSBHandle, &usb->rxurbs[urb_num].ovl, &bytes_transfered, false)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            ResetEvent(usb->rxurbs[urb_num].ovl.hEvent);
        } else {
            usb->rxurbs[urb_num].pending = false;
            candle_prepare_read(usb, urb_num);
        }
        candle_logf(L"read result failed urb=%u winerr=%lu", urb_num, err);
        return CANDLE_ERR_READ_RESULT;
    }
    usb->rxurbs[urb_num].pending = false;

    const DWORD copy_len = bytes_transfered < bufsize ? bytes_transfered : bufsize;
    memcpy(buf, usb->rxurbs[urb_num].buf, copy_len);
    *len = copy_len;

    if (!candle_prepare_read(usb, urb_num)) {
        return CANDLE_ERR_PREPARE_READ;
    }
    return CANDLE_ERR_OK;
}
