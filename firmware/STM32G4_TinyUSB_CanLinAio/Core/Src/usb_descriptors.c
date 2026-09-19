/*
 * usb_descriptors.c — USB descriptors for the gs_usb (CAN) / lin_usb (LIN) /
 * aio_usb (I/O + analog) adapter.
 *
 * Three vendor-specific bulk interfaces, one per class driver registered via
 * usbd_app_driver_get_cb() in usb_app_drivers.c.  Which interfaces are present
 * is controlled by GS_USB_ENABLED / LIN_USB_ENABLED / AIO_USB_ENABLED in
 * usb_app_config.h.
 *
 * VID 0x1d50 / PID 0x606f (OpenMoko / candleLight) is what the Linux kernel
 * gs_usb driver matches to bind the CAN interface without any host-side
 * configuration.  Changing it breaks that.
 *
 * Windows: a BOS / MS OS 2.0 descriptor set gives every interface the WinUSB
 * compatible ID and its own DeviceInterfaceGUID, so no INF or Zadig step is
 * needed.
 */
#include "tusb.h"
#include "stm32g473xx.h"
#include "usb_app_config.h"

#if GS_USB_ENABLED
#include "gs_usb_config.h"
#endif
#if LIN_USB_ENABLED
#include "lin_usb_config.h"
#endif
#if AIO_USB_ENABLED
#include "aio_usb_config.h"
#endif

#include <string.h>

#define USB_VID   0x1d50u
#define USB_PID   0x606fu

// The Linux gs_usb driver binds interface 0 of 1d50:606f unconditionally.
// Without the CAN interface, LIN or AIO moves to interface 0 and the kernel
// would try to drive it as a CAN adapter: use your own VID/PID then.
#if !GS_USB_ENABLED && (USB_VID == 0x1d50u) && (USB_PID == 0x606fu)
#warning "GS_USB_ENABLED is 0 but VID/PID is still candleLight 1d50:606f: Linux gs_usb will claim interface 0"
#endif
#define USB_BCD   0x0210u   // 2.1 required for the BOS descriptor (MS OS 2.0)

//--------------------------------------------------------------------+
// Interface numbering — derived from which drivers are enabled
//--------------------------------------------------------------------+
#if GS_USB_ENABLED
#  define ITF_NUM_CAN  0
#endif

#if LIN_USB_ENABLED
#  define ITF_NUM_LIN  GS_USB_ENABLED   /* 0 if CAN disabled, 1 if both */
#endif

#if AIO_USB_ENABLED
#  define ITF_NUM_AIO  (GS_USB_ENABLED + LIN_USB_ENABLED)  /* after CAN+LIN */
#endif

#define ITF_NUM_TOTAL  (GS_USB_ENABLED + LIN_USB_ENABLED + AIO_USB_ENABLED)

//--------------------------------------------------------------------+
// String indices
//--------------------------------------------------------------------+
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CAN_ITF,   // 4
    STRID_LIN_ITF,   // 5
    STRID_AIO_ITF,   // 6
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    // Windows caches the MS OS 2.0 set per VID/PID/bcdDevice: bump this
    // whenever the descriptor set changes (0x0102: one function subset per
    // interface instead of CAN only).
    .bcdDevice          = 0x0102,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//
// Each active interface contributes: interface(9) + EP_IN(7) + EP_OUT(7) = 23 bytes
//--------------------------------------------------------------------+
#define _ITF_LEN   (9u + 7u + 7u)
#define CONFIG_TOTAL_LEN  \
    (TUD_CONFIG_DESC_LEN + (GS_USB_ENABLED * _ITF_LEN) + \
     (LIN_USB_ENABLED * _ITF_LEN) + (AIO_USB_ENABLED * _ITF_LEN))

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

#if GS_USB_ENABLED
    9, TUSB_DESC_INTERFACE, ITF_NUM_CAN, 0, 2, 0xFF, 0xFF, 0xFF, STRID_CAN_ITF,
    7, TUSB_DESC_ENDPOINT,  GS_USB_EP_IN,  TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
    7, TUSB_DESC_ENDPOINT,  GS_USB_EP_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
#endif

#if LIN_USB_ENABLED
    9, TUSB_DESC_INTERFACE, ITF_NUM_LIN, 0, 2, 0xFF, 0xFF, 0x01, STRID_LIN_ITF,
    7, TUSB_DESC_ENDPOINT,  LIN_USB_EP_IN,  TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
    7, TUSB_DESC_ENDPOINT,  LIN_USB_EP_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
#endif

#if AIO_USB_ENABLED
    9, TUSB_DESC_INTERFACE, ITF_NUM_AIO, 0, 2, 0xFF, 0xFF, 0x02, STRID_AIO_ITF,
    7, TUSB_DESC_ENDPOINT,  AIO_USB_EP_IN,  TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
    7, TUSB_DESC_ENDPOINT,  AIO_USB_EP_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
#endif
};

//--------------------------------------------------------------------+
// BOS + MS OS 2.0 Descriptors (Windows WinUSB auto-detection, every interface)
//--------------------------------------------------------------------+

#define _BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, MS_OS_20_VENDOR_CODE),
};

/*
 * Function subset for interface `itf` (MS_OS_20_FN_LEN bytes in total):
 *   - function subset header: wLength, wType, bFirstInterface, bReserved, wSubsetLength
 *   - compatible ID "WINUSB", sub-compatible ID zeros
 *   - registry property header up to the value: wLength=128, wType=REG_PROPERTY,
 *     wPropertyDataType=REG_SZ(1), wPropertyNameLength=40, "DeviceInterfaceGUID\0"
 *     (UTF-16), wPropertyDataLength=78
 * The 78-byte value, "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\0" in UTF-16, must
 * follow directly. Every interface needs its own GUID.
 */
#define MS_OS_20_FUNCTION_HEAD(itf)                                                   \
    U16_TO_U8S_LE(0x0008u), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),           \
    (itf), 0x00, U16_TO_U8S_LE(MS_OS_20_FN_LEN),                                      \
    U16_TO_U8S_LE(0x0014u), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),             \
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,                                         \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                   \
    U16_TO_U8S_LE(0x0080u), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),             \
    U16_TO_U8S_LE(0x0001u), U16_TO_U8S_LE(0x0028u),                                   \
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,                      \
    'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0, 0,0,                       \
    U16_TO_U8S_LE(0x004Eu)

/* Exposed to usb_app_drivers.c for the vendor-request response. */
uint8_t const desc_ms_os_20[MS_OS_20_DESC_LEN] = {
    // Set header: wLength, wType, dwWindowsVersion (Win 8.1+), wTotalLength
    U16_TO_U8S_LE(0x000Au), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000u), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: wLength, wType, bConfigurationValue, bReserved, wTotalLength
    U16_TO_U8S_LE(0x0008u), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0x00, 0x00, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0Au),

#if GS_USB_ENABLED
    // CAN: {c15b4308-04d3-11e6-b3ea-6057189e6443} is the standard candleLight/gs_usb
    // GUID; it must match what candle_list_scan() looks for.
    MS_OS_20_FUNCTION_HEAD(ITF_NUM_CAN),
    '{',0,'c',0,'1',0,'5',0,'b',0,'4',0,'3',0,'0',0,'8',0,'-',0,
    '0',0,'4',0,'d',0,'3',0,'-',0,'1',0,'1',0,'e',0,'6',0,'-',0,
    'b',0,'3',0,'e',0,'a',0,'-',0,'6',0,'0',0,'5',0,'7',0,'1',0,
    '8',0,'9',0,'e',0,'6',0,'4',0,'4',0,'3',0,'}',0, 0,0,
#endif

#if LIN_USB_ENABLED
    // LIN: {dfaa1f65-e194-414c-ac5d-66ea6a8ba9c9}. Must differ from the CAN GUID,
    // otherwise candle_api would also try to open this interface as a CAN channel.
    MS_OS_20_FUNCTION_HEAD(ITF_NUM_LIN),
    '{',0,'d',0,'f',0,'a',0,'a',0,'1',0,'f',0,'6',0,'5',0,'-',0,
    'e',0,'1',0,'9',0,'4',0,'-',0,'4',0,'1',0,'4',0,'c',0,'-',0,
    'a',0,'c',0,'5',0,'d',0,'-',0,'6',0,'6',0,'e',0,'a',0,'6',0,
    'a',0,'8',0,'b',0,'a',0,'9',0,'c',0,'9',0,'}',0, 0,0,
#endif

#if AIO_USB_ENABLED
    // AIO: {4c86c041-3321-446b-ba72-6a4be9f1c2b0}
    MS_OS_20_FUNCTION_HEAD(ITF_NUM_AIO),
    '{',0,'4',0,'c',0,'8',0,'6',0,'c',0,'0',0,'4',0,'1',0,'-',0,
    '3',0,'3',0,'2',0,'1',0,'-',0,'4',0,'4',0,'6',0,'b',0,'-',0,
    'b',0,'a',0,'7',0,'2',0,'-',0,'6',0,'a',0,'4',0,'b',0,'e',0,
    '9',0,'f',0,'1',0,'c',0,'2',0,'b',0,'0',0,'}',0, 0,0,
#endif
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size mismatch");

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },  // 0: English (0x0409)
    "STM32G473",                    // 1: Manufacturer

#if GS_USB_ENABLED && LIN_USB_ENABLED && AIO_USB_ENABLED
    "CAN+LIN+AIO USB Adapter",
#elif GS_USB_ENABLED && LIN_USB_ENABLED
    "CAN+LIN USB Adapter",
#elif GS_USB_ENABLED && AIO_USB_ENABLED
    "CAN+AIO USB Adapter",
#elif LIN_USB_ENABLED && AIO_USB_ENABLED
    "LIN+AIO USB Adapter",
#elif GS_USB_ENABLED
    "CAN USB Adapter",
#elif LIN_USB_ENABLED
    "LIN USB Adapter",
#else
    "AIO USB Adapter",
#endif

    NULL,                           // 3: Serial (generated below)
    "CAN Interface",                // 4: CAN interface string
    "LIN Interface",                // 5: LIN interface string
    "AIO Interface",                // 6: AIO interface string
};

static uint16_t _desc_str[32 + 1];

static size_t make_serial(uint16_t *out, size_t max_chars) {
    uint32_t const *uid = (uint32_t const *)UID_BASE;
    uint8_t  raw[12];
    memcpy(raw,     &uid[0], 4);
    memcpy(raw + 4, &uid[1], 4);
    memcpy(raw + 8, &uid[2], 4);

    static const char hex[] = "0123456789ABCDEF";
    size_t n = (sizeof(raw) * 2u < max_chars) ? sizeof(raw) : max_chars / 2u;
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[(raw[i] >> 4) & 0xFu];
        out[i * 2 + 1] = hex[raw[i] & 0xFu];
    }
    return n * 2u;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        chr_count = make_serial(_desc_str + 1, 32);
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr)) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t max = TU_ARRAY_SIZE(_desc_str) - 1u;
        if (chr_count > max) chr_count = max;
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return _desc_str;
}
