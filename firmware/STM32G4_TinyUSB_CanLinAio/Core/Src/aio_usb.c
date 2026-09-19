#include "aio_usb.h"
#include "device/usbd_pvt.h"
#include <string.h>

/* The bulk IN auto-report frame and the READ_STATUS control response share the
 * aio_usb_report_t layout; it must fit in a single full-speed packet (and in
 * the 64-byte _ctrl_buf union).  Catches AIO_USB_ANALOG_COUNT being raised too
 * far at compile time. */
TU_VERIFY_STATIC(sizeof(aio_usb_report_t) <= 64,
                 "aio_usb_report_t exceeds one 64-byte USB packet");

/* -------------------------------------------------------------------------
 * Critical-section helpers
 *
 * The IN queue, _in_busy flag and per-line report timers are touched from the
 * USB stack (aiousb_xfer_cb()), the main loop (aio_usb_task()) and potentially
 * a hardware backend interrupt.  Wrap shared-state access in a PRIMASK
 * save/restore critical section (nesting-safe).
 * ------------------------------------------------------------------------- */
static inline uint32_t cs_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void cs_exit(uint32_t primask)
{
    __set_PRIMASK(primask);
}

/* -------------------------------------------------------------------------
 * Per-line runtime state
 * ------------------------------------------------------------------------- */
typedef struct
{
    uint8_t  mode;           /* AIO_USB_IO_MODE_INPUT / OUTPUT */
    uint8_t  flags;          /* AIO_USB_IO_FLAG_*              */
    uint8_t  default_state;  /* configured default output level */
    /* volatile: written from the control DATA stage and read by aio_usb_task(),
     * which may run in a different context. */
    volatile uint32_t auto_report_ms; /* 0 = no periodic report          */
    volatile uint32_t next_report_ms; /* HAL_GetTick() of next due report */
} aio_usb_io_t;

/* true between USB suspend and resume: no auto-reports are generated */
static bool _suspended;

/* -------------------------------------------------------------------------
 * Module-level state
 * ------------------------------------------------------------------------- */
static aio_usb_io_t _io[AIO_USB_IO_COUNT];

static struct
{
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t rhport;
} _usb;

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN union
{
    aio_usb_host_config_t host_config;
    aio_usb_caps_t        caps;
    aio_usb_io_config_t   io_config;
    aio_usb_io_set_t      io_set;
    aio_usb_report_t      report;
    uint32_t              timestamp;
    uint8_t               raw[64];
} _ctrl_buf;

/* Index of the I/O line addressed by the in-flight IO_CONFIG control transfer */
static uint8_t _ctrl_io;

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN aio_usb_io_set_t _out_frame;

static aio_usb_report_t _in_queue[AIO_USB_IN_QUEUE_SIZE];
static uint8_t _in_head;
static uint8_t _in_tail;
static bool    _in_busy;

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN aio_usb_report_t _in_buf;

/* -------------------------------------------------------------------------
 * In-queue helpers (circular buffer, same pattern as lin_usb/gs_usb)
 * ------------------------------------------------------------------------- */
static bool in_queue_push(const aio_usb_report_t *report)
{
    uint32_t pm = cs_enter();
    uint8_t next = (uint8_t)((_in_head + 1u) % AIO_USB_IN_QUEUE_SIZE);
    if (next == _in_tail)
    {
        cs_exit(pm);
        return false; /* queue full */
    }
    _in_queue[_in_head] = *report;
    _in_head = next;
    cs_exit(pm);
    return true;
}

static void in_try_send(void)
{
    /* Before SET_CONFIGURATION (and right after a bus reset) ep_in is 0: a
     * transfer queued now would land on EP0.  Reports stay queued until then. */
    if (!tud_mounted() || _usb.ep_in == 0u)
    {
        return;
    }

    /* Atomically claim the IN endpoint and copy the oldest report; the transfer
     * is started outside the critical section to keep interrupts disabled only
     * briefly.  The report leaves the queue only once the transfer is running. */
    uint32_t pm = cs_enter();
    if (_in_busy || _in_tail == _in_head)
    {
        cs_exit(pm);
        return;
    }
    _in_buf  = _in_queue[_in_tail];
    _in_busy = true;
    cs_exit(pm);

    if (usbd_edpt_xfer(_usb.rhport, _usb.ep_in,
                       (uint8_t *)&_in_buf, sizeof(_in_buf), false))
    {
        /* Only the _in_busy owner moves the tail, so nothing pops concurrently. */
        pm = cs_enter();
        _in_tail = (uint8_t)((_in_tail + 1u) % AIO_USB_IN_QUEUE_SIZE);
        cs_exit(pm);
    }
    else
    {
        /* Not started: keep the report queued and release the endpoint so
         * aio_usb_task() retries, instead of leaving _in_busy stuck forever. */
        _in_busy = false;
    }
}

/* -------------------------------------------------------------------------
 * Snapshot helpers
 * ------------------------------------------------------------------------- */

/* Bitmask of lines currently configured as outputs. */
static uint32_t io_direction_mask(void)
{
    uint32_t dir = 0u;
    for (uint8_t i = 0; i < AIO_USB_IO_COUNT; i++)
    {
        if (_io[i].mode == AIO_USB_IO_MODE_OUTPUT)
        {
            dir |= (1u << i);
        }
    }
    return dir;
}

/* Fill a report with the current state of every line and analog channel. */
static void build_snapshot(aio_usb_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->timestamp_ms = HAL_GetTick();
    r->io_states    = aio_hw_read_inputs();
    r->io_direction = io_direction_mask();
    for (uint8_t ch = 0; ch < AIO_USB_ANALOG_COUNT; ch++)
    {
        r->analog[ch] = aio_hw_read_analog(ch);
    }
}

/* -------------------------------------------------------------------------
 * USB reset helper
 * ------------------------------------------------------------------------- */
static void reset_usb_state(void)
{
    _in_busy    = false;
    _in_head    = 0;
    _in_tail    = 0;
    _usb.ep_in  = 0;
    _usb.ep_out = 0;
    _suspended  = false;
}

/* -------------------------------------------------------------------------
 * TinyUSB class driver callbacks
 * ------------------------------------------------------------------------- */
static void aiousb_init(void)
{
    /* GPIO/ADC hardware is initialised by the application; nothing to do here */
}

static bool aiousb_deinit(void)
{
    reset_usb_state();
    return true;
}

static void aiousb_reset(uint8_t rhport)
{
    (void)rhport;
    reset_usb_state();
}

static uint16_t aiousb_open(uint8_t rhport,
                            tusb_desc_interface_t const *desc_intf,
                            uint16_t max_len)
{
    (void)max_len;

    /* Protocol 0x02 distinguishes the AIO interface from CAN (0xFF) and
     * LIN (0x01), so aiousb_open() only claims its own interface when all
     * drivers are registered. */
    TU_VERIFY(desc_intf->bInterfaceClass    == TUSB_CLASS_VENDOR_SPECIFIC, 0);
    TU_VERIFY(desc_intf->bInterfaceSubClass == 0xFFu, 0);
    TU_VERIFY(desc_intf->bInterfaceProtocol == 0x02u, 0);

    _usb.itf_num = desc_intf->bInterfaceNumber;
    _usb.rhport  = rhport;

    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p = tu_desc_next(desc_intf);

    for (uint8_t i = 0; i < desc_intf->bNumEndpoints; i++)
    {
        TU_ASSERT(tu_desc_type(p) == TUSB_DESC_ENDPOINT, 0);
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);

        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
        {
            _usb.ep_in = ep->bEndpointAddress;
        }
        else
        {
            _usb.ep_out = ep->bEndpointAddress;
        }

        drv_len += tu_desc_len(p);
        p        = tu_desc_next(p);
    }

    /* Arm the OUT endpoint for the first output-set frame from the host */
    usbd_edpt_xfer(rhport, _usb.ep_out,
                   (uint8_t *)&_out_frame, sizeof(_out_frame), false);

    return drv_len;
}

/*
 * Called by the shared tud_vendor_control_xfer_cb dispatcher in
 * usb_app_drivers.c.  Returns true if this driver claimed the request.
 * SETUP stage: validates the request and prepares IN data or arms the
 * data-OUT stage.  DATA stage: applies received configuration.
 */
bool aiousb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                            tusb_control_request_t const *req)
{
    /* Only handle vendor requests addressed to our interface */
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR)
    {
        return false;
    }
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE)
    {
        return false;
    }
    /* Interface-recipient requests carry the interface number in wIndex; the
     * high byte must be zero per USB 2.0 §9.3.4, so compare the full field. */
    if (req->wIndex != _usb.itf_num)
    {
        return false;
    }

    if (stage == CONTROL_STAGE_SETUP)
    {
        switch ((aio_usb_breq_t)req->bRequest)
        {
        /* ------ OUT requests (host sends data to device) ------ */
        case AIO_USB_BREQ_HOST_FORMAT:
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(aio_usb_host_config_t));

        case AIO_USB_BREQ_IDENTIFY:
            /* No data payload — acknowledge with a zero-length status stage.
             * Arming a fixed 1-byte data stage would STALL EP0 when the host
             * sends wLength = 0. */
            aio_hw_identify();
            return tud_control_status(rhport, req);

        case AIO_USB_BREQ_IO_CONFIG:
        {
            uint8_t io = (uint8_t)(req->wValue & 0xFFu);
            if (io >= AIO_USB_IO_COUNT)
            {
                return false;
            }
            _ctrl_io = io;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(aio_usb_io_config_t));
        }

        case AIO_USB_BREQ_IO_SET:
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(aio_usb_io_set_t));

        /* ------ IN requests (device sends data to host) ------ */
        case AIO_USB_BREQ_DEVICE_CONFIG:
        {
            aio_usb_caps_t *caps = &_ctrl_buf.caps;
            memset(caps, 0, sizeof(*caps));
            caps->io_count               = AIO_USB_IO_COUNT;
            caps->analog_count           = AIO_USB_ANALOG_COUNT;
            caps->analog_resolution_bits = AIO_USB_ANALOG_RES_BITS;
            caps->io_input_capable_mask  = AIO_USB_IO_INPUT_MASK;
            caps->io_output_capable_mask = AIO_USB_IO_OUTPUT_MASK;
            caps->sw_version             = AIO_USB_SW_VERSION;
            caps->hw_version             = AIO_USB_HW_VERSION;
            caps->features               = AIO_USB_FEATURE_TIMESTAMP |
                                           AIO_USB_FEATURE_AUTO_REPORT;
#if AIO_USB_ANALOG_COUNT > 0
            caps->features |= AIO_USB_FEATURE_ANALOG;
#endif
            return tud_control_xfer(rhport, req, caps, sizeof(*caps));
        }

        case AIO_USB_BREQ_READ_STATUS:
        {
            aio_usb_report_t *r = &_ctrl_buf.report;
            build_snapshot(r);
            return tud_control_xfer(rhport, req, r, sizeof(*r));
        }

        case AIO_USB_BREQ_TIMESTAMP:
        {
            uint32_t ts = HAL_GetTick();
            memcpy(_ctrl_buf.raw, &ts, sizeof(ts));
            return tud_control_xfer(rhport, req, _ctrl_buf.raw, sizeof(ts));
        }

        default:
            return false;
        }
    }

    if (stage == CONTROL_STAGE_DATA)
    {
        switch ((aio_usb_breq_t)req->bRequest)
        {
        case AIO_USB_BREQ_IO_CONFIG:
        {
            aio_usb_io_config_t *cfg = &_ctrl_buf.io_config;
            /* Re-validate the line saved at SETUP and reject unknown modes
             * before storing host-supplied values. */
            if (_ctrl_io >= AIO_USB_IO_COUNT ||
                cfg->mode > AIO_USB_IO_MODE_OUTPUT)
            {
                break;
            }
            aio_usb_io_t *line = &_io[_ctrl_io];
            line->mode          = cfg->mode;
            line->flags         = cfg->flags;
            line->default_state = cfg->default_state;
            line->auto_report_ms = cfg->auto_report_ms;
            /* next_report_ms is only consumed while auto_report_ms != 0; keep it
             * coherent so a stale value can't fire if the guard ever changes. */
            line->next_report_ms = (cfg->auto_report_ms != 0u)
                                   ? (HAL_GetTick() + cfg->auto_report_ms)
                                   : 0u;
            aio_hw_config_io(_ctrl_io, cfg->mode, cfg->flags, cfg->default_state);
            /* Apply the default level immediately for output lines */
            if (cfg->mode == AIO_USB_IO_MODE_OUTPUT)
            {
                uint32_t bit = (1u << _ctrl_io);
                aio_hw_set_outputs(bit, cfg->default_state ? bit : 0u);
            }
            break;
        }

        case AIO_USB_BREQ_IO_SET:
            aio_hw_set_outputs(_ctrl_buf.io_set.mask, _ctrl_buf.io_set.values);
            break;

        default:
            break;
        }
        return true;
    }

    /* ACK stage: nothing to do */
    return true;
}

/*
 * Bulk transfer completion callback.
 * EP_OUT: received an output-set frame from the host.
 * EP_IN:  finished sending a report to the host.
 */
static bool aiousb_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                           xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr == _usb.ep_out)
    {
        if (result == XFER_RESULT_SUCCESS &&
            xferred_bytes == sizeof(aio_usb_io_set_t))
        {
            aio_hw_set_outputs(_out_frame.mask, _out_frame.values);
        }

        /* Re-arm OUT endpoint for the next frame from the host */
        usbd_edpt_xfer(rhport, _usb.ep_out,
                       (uint8_t *)&_out_frame, sizeof(_out_frame), false);
    }
    else if (ep_addr == _usb.ep_in)
    {
        _in_busy = false;
        in_try_send();
    }

    return true;
}

/* -------------------------------------------------------------------------
 * Driver registration
 * Non-static so usb_app_drivers.c can build the shared driver table.
 * ------------------------------------------------------------------------- */
const usbd_class_driver_t aio_usb_driver = {
    .name            = "AIO_USB",
    .init            = aiousb_init,
    .deinit          = aiousb_deinit,
    .reset           = aiousb_reset,
    .open            = aiousb_open,
    .control_xfer_cb = NULL,  /* vendor requests routed via tud_vendor_control_xfer_cb */
    .xfer_cb         = aiousb_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void aio_usb_init(void)
{
    memset(_io, 0, sizeof(_io));
    memset(&_usb, 0, sizeof(_usb));
    _in_head = 0;
    _in_tail = 0;
    _in_busy = false;
}

void aio_usb_suspend(void)
{
    _suspended = true;
}

void aio_usb_resume(void)
{
    /* Restart every report timer from now: reports that fell due while
     * suspended would otherwise all fire at once. */
    uint32_t now = HAL_GetTick();
    for (uint8_t i = 0; i < AIO_USB_IO_COUNT; i++)
    {
        if (_io[i].auto_report_ms != 0u)
        {
            _io[i].next_report_ms = now + _io[i].auto_report_ms;
        }
    }
    _suspended = false;
}

void aio_usb_task(void)
{
    /* Only touch endpoints once configured (tud_mounted): before
     * SET_CONFIGURATION the endpoint addresses are still 0 (EP0). */
    if (!tud_mounted() || _suspended)
    {
        return;
    }

    /*
     * Auto-report: each line may carry its own period.  When any line's timer
     * is due, emit a single full snapshot (all lines + all analog channels)
     * and advance every due timer.  Lines sharing a period therefore coalesce
     * into one frame per period.
     */
    uint32_t now = HAL_GetTick();
    bool report_due = false;

    for (uint8_t i = 0; i < AIO_USB_IO_COUNT; i++)
    {
        if (_io[i].auto_report_ms == 0u)
        {
            continue;
        }
        if ((int32_t)(now - _io[i].next_report_ms) >= 0)
        {
            report_due = true;
            _io[i].next_report_ms = now + _io[i].auto_report_ms;
        }
    }

    if (report_due)
    {
        aio_usb_report_t snapshot;
        build_snapshot(&snapshot);
        in_queue_push(&snapshot);
    }

    in_try_send();
}

/* -------------------------------------------------------------------------
 * Weak hardware backend defaults
 *
 * These no-op / zero implementations let the project link and enumerate
 * before any real GPIO/ADC is wired.  Override them in the application
 * (define a non-weak symbol with the same signature) to drive actual pins.
 * ------------------------------------------------------------------------- */
__attribute__((weak)) void aio_hw_config_io(uint8_t io, uint8_t mode,
                                            uint8_t flags, uint8_t default_state)
{
    (void)io; (void)mode; (void)flags; (void)default_state;
}

__attribute__((weak)) void aio_hw_set_outputs(uint32_t mask, uint32_t values)
{
    (void)mask; (void)values;
}

__attribute__((weak)) uint32_t aio_hw_read_inputs(void)
{
    return 0u;
}

__attribute__((weak)) uint16_t aio_hw_read_analog(uint8_t ch)
{
    (void)ch;
    return 0u;
}

__attribute__((weak)) void aio_hw_identify(void)
{
}
