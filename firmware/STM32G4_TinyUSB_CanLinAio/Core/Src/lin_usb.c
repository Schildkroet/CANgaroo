#include "lin_usb.h"
#include "device/usbd_pvt.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Critical-section helpers
 *
 * The IN queue and _in_busy flag are touched from the LIN engine
 * (lin_usb_report_frame(), typically a UART interrupt), the USB stack
 * (linusb_xfer_cb()) and the main loop (lin_usb_task()).  All access to that
 * shared state must be atomic, so wrap it in a PRIMASK save/restore critical
 * section (nesting-safe).
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
 * This driver is pure USB plumbing.
 *
 * It relays host vendor requests to the LIN engine (the lin_engine_* hooks,
 * weak no-op stubs at the bottom of this file) and streams frames the engine
 * reports back — via lin_usb_report_frame() — to the host over bulk IN.
 *
 * All LIN scheduling, timing, master/slave behaviour and bus (UART) access
 * live in the engine, which is integrated separately and overrides the weak
 * hooks.  Nothing in this file touches a peripheral.
 * ------------------------------------------------------------------------- */

static struct
{
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t rhport;
} _usb;

/* Channel addressed by the in-flight control transfer (set in SETUP). */
static uint8_t _ctrl_ch;

static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN union
{
    lin_usb_host_config_t    host_config;
    lin_usb_device_config_t  device_config;
    lin_usb_bus_config_t     baudrate;
    lin_usb_mode_t           mode;
    lin_usb_schedule_entry_t schedule_entry;
    lin_usb_sleep_wakeup_t   sleep_wakeup;
    lin_usb_bus_state_t      bus_state;
    uint32_t                 timestamp;
    uint8_t                  raw[64];
} _ctrl_buf;

/* Bulk OUT buffer: the host's "set frame" payload. */
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN lin_usb_host_frame_t _out_frame;

/* true while the OUT endpoint is armed for the next "set frame" */
static bool _out_armed;

/* Set-data ACK that did not fit into the IN queue yet.  While it is pending the
 * OUT endpoint stays un-armed (the host is NAKed), so every "set frame" packet
 * gets exactly one ACK even when bus traffic has filled the queue. */
static lin_usb_host_frame_t _ack;
static bool                 _ack_pending;

/* Frames dropped per channel because the IN queue was full; reported to the
 * host in the BUS_STATE reply. */
static uint16_t _rx_dropped[LIN_USB_CHANNEL_COUNT];

/* Bulk IN queue: frames the engine reported, waiting to go to the host. */
static lin_usb_host_frame_t _in_queue[LIN_USB_IN_QUEUE_SIZE];
static uint8_t _in_head;
static uint8_t _in_tail;
static bool    _in_busy;
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN lin_usb_host_frame_t _in_buf;

/* -------------------------------------------------------------------------
 * In-queue helpers (circular buffer)
 * ------------------------------------------------------------------------- */
/* Push a frame; if the queue is full and `count_drop` is set, count it as a
 * dropped bus frame for its channel (inside the same critical section, since
 * frames may be reported from interrupt context). */
static bool in_queue_push(const lin_usb_host_frame_t *frame, bool count_drop)
{
    uint32_t pm = cs_enter();
    uint8_t next = (uint8_t)((_in_head + 1u) % LIN_USB_IN_QUEUE_SIZE);
    if (next == _in_tail)
    {
        if (count_drop && frame->channel < LIN_USB_CHANNEL_COUNT &&
            _rx_dropped[frame->channel] < UINT16_MAX)
        {
            _rx_dropped[frame->channel]++;
        }
        cs_exit(pm);
        return false; /* queue full */
    }
    _in_queue[_in_head] = *frame;
    _in_head = next;
    cs_exit(pm);
    return true;
}

static void in_try_send(void)
{
    /* Before SET_CONFIGURATION (and right after a bus reset) ep_in is 0: a
     * transfer queued now would land on EP0.  Frames stay queued until then. */
    if (!tud_mounted() || _usb.ep_in == 0u)
    {
        return;
    }

    /* Atomically claim the IN endpoint and copy the oldest frame; the transfer
     * is started outside the critical section to keep interrupts disabled only
     * briefly.  The frame leaves the queue only once the transfer is running. */
    uint32_t pm = cs_enter();
    if (_in_busy || _in_tail == _in_head)
    {
        cs_exit(pm);
        return;
    }
    _in_buf  = _in_queue[_in_tail];
    _in_busy = true;
    cs_exit(pm);

    /* lin_usb_report_frame() may run in an interrupt: tell TinyUSB which
     * context starts the transfer. */
    if (usbd_edpt_xfer(_usb.rhport, _usb.ep_in,
                       (uint8_t *)&_in_buf, sizeof(_in_buf), __get_IPSR() != 0u))
    {
        /* Only the _in_busy owner moves the tail, so nothing pops concurrently. */
        pm = cs_enter();
        _in_tail = (uint8_t)((_in_tail + 1u) % LIN_USB_IN_QUEUE_SIZE);
        cs_exit(pm);
    }
    else
    {
        /* Not started: keep the frame queued and release the endpoint so
         * lin_usb_task() retries, instead of leaving _in_busy stuck forever. */
        _in_busy = false;
    }
}

/* (Re)arm the OUT endpoint, tracking success so lin_usb_task() can retry if the
 * stack rejected the request instead of going deaf to host frames. */
static void out_arm(void)
{
    _out_armed = usbd_edpt_xfer(_usb.rhport, _usb.ep_out,
                                (uint8_t *)&_out_frame, sizeof(_out_frame), false);
}

/* -------------------------------------------------------------------------
 * USB reset helper
 * ------------------------------------------------------------------------- */
static void reset_usb_state(void)
{
    _in_busy    = false;
    _in_head    = 0;
    _in_tail    = 0;
    _out_armed  = false;
    _ack_pending = false;
    _usb.ep_in  = 0;
    _usb.ep_out = 0;
    memset(_rx_dropped, 0, sizeof(_rx_dropped));
}

/*
 * Queue a frame for the host.  `count_drop` is true only for frames that came
 * off the LIN bus: the BUS_STATE `dropped` counter reports lost bus traffic, so
 * locally generated packets (such as set-data ACKs) must not inflate it.
 */
static bool report_frame(const lin_usb_host_frame_t *frame, bool count_drop)
{
    bool ok = in_queue_push(frame, count_drop);
    in_try_send();
    return ok;
}

/* -------------------------------------------------------------------------
 * TinyUSB class driver callbacks
 * ------------------------------------------------------------------------- */
static void linusb_init(void)
{
    /* Nothing to do — the LIN engine is initialised by the application. */
}

static bool linusb_deinit(void)
{
    reset_usb_state();
    return true;
}

static void linusb_reset(uint8_t rhport)
{
    (void)rhport;
    reset_usb_state();
    /* The host session ended without MODE STOP; stop channels in the engine. */
    lin_engine_reset();
}

static uint16_t linusb_open(uint8_t rhport,
                             tusb_desc_interface_t const *desc_intf,
                             uint16_t max_len)
{
    (void)max_len;

    /* Protocol 0x01 distinguishes the LIN interface from CAN (0xFF) / AIO (0x02). */
    TU_VERIFY(desc_intf->bInterfaceClass    == TUSB_CLASS_VENDOR_SPECIFIC, 0);
    TU_VERIFY(desc_intf->bInterfaceSubClass == 0xFFu, 0);
    TU_VERIFY(desc_intf->bInterfaceProtocol == 0x01u, 0);

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

    /* Arm the OUT endpoint for the first "set frame" from the host. */
    out_arm();

    return drv_len;
}

/*
 * Vendor control handler (called by the shared dispatcher in usb_app_drivers.c).
 * SETUP stage: validate and arm the data stage / build the IN response.
 * DATA stage:  relay the received configuration to the LIN engine.
 */
bool linusb_control_xfer_cb(uint8_t rhport, uint8_t stage,
                             tusb_control_request_t const *req)
{
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

    uint8_t ch = (uint8_t)(req->wValue & 0xFFu);

    if (stage == CONTROL_STAGE_SETUP)
    {
        switch ((lin_usb_breq_t)req->bRequest)
        {
        /* ------ OUT requests (host sends data to device) ------ */
        case LIN_USB_BREQ_HOST_FORMAT:
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_host_config_t));

        case LIN_USB_BREQ_BAUDRATE:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_bus_config_t));

        case LIN_USB_BREQ_MODE:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_mode_t));

        case LIN_USB_BREQ_IDENTIFY:
            /* No data payload — acknowledge with a zero-length status stage.
             * Arming a fixed 1-byte data stage would STALL EP0 when the host
             * sends wLength = 0. */
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            lin_engine_identify(ch);
            return tud_control_status(rhport, req);

        case LIN_USB_BREQ_FRAME_CONFIG:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_schedule_entry_t));

        case LIN_USB_BREQ_SCHEDULE:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            /* wValue[15:8] = slot index; reject before the data stage */
            if (((req->wValue >> 8) & 0xFFu) >= LIN_USB_MAX_SCHEDULE_ENTRIES) { return false; }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_schedule_entry_t));

        case LIN_USB_BREQ_SLEEP_WAKEUP:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            _ctrl_ch = ch;
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf, sizeof(lin_usb_sleep_wakeup_t));

        /* ------ IN requests (device sends data to host) ------ */
        case LIN_USB_BREQ_DEVICE_CONFIG:
        {
            lin_usb_device_config_t *cfg = &_ctrl_buf.device_config;
            memset(cfg, 0, sizeof(*cfg));
            cfg->schedule_tables     = LIN_USB_MAX_SCHEDULE_TABLES;
            cfg->schedule_entries    = LIN_USB_MAX_SCHEDULE_ENTRIES;
            cfg->supported_baudrates = LIN_USB_SUPPORTED_BAUDRATES;
            cfg->icount              = LIN_USB_CHANNEL_COUNT - 1u;
            cfg->sw_version          = LIN_USB_SW_VERSION;
            cfg->hw_version          = LIN_USB_HW_VERSION;
            cfg->features |= LIN_USB_FEATURE_SCHEDULING;
#if LIN_USB_CUSTOM_BAUDRATE_SUPPORTED
            cfg->features |= LIN_USB_FEATURE_CUSTOM_BAUDRATE;
#endif
            cfg->features |= LIN_USB_FEATURE_TIMESTAMP;
            cfg->features |= LIN_USB_FEATURE_BUS_STATE;
            cfg->features |= LIN_USB_FEATURE_LISTEN_ONLY;
            return tud_control_xfer(rhport, req, cfg, sizeof(*cfg));
        }

        case LIN_USB_BREQ_TIMESTAMP:
        {
            uint32_t ts = HAL_GetTick();
            memcpy(_ctrl_buf.raw, &ts, sizeof(ts));
            return tud_control_xfer(rhport, req, _ctrl_buf.raw, sizeof(ts));
        }

        case LIN_USB_BREQ_BUS_STATE:
            if (ch >= LIN_USB_CHANNEL_COUNT) { return false; }
            memset(&_ctrl_buf.bus_state, 0, sizeof(_ctrl_buf.bus_state));
            lin_engine_get_bus_state(ch, &_ctrl_buf.bus_state);
            _ctrl_buf.bus_state.dropped = _rx_dropped[ch];
            return tud_control_xfer(rhport, req,
                                    &_ctrl_buf.bus_state, sizeof(_ctrl_buf.bus_state));

        default:
            return false;
        }
    }

    if (stage == CONTROL_STAGE_DATA)
    {
        /* Re-validate the channel saved at SETUP: a host that aborts and
         * retries between stages could leave _ctrl_ch stale here. */
        if (_ctrl_ch >= LIN_USB_CHANNEL_COUNT)
        {
            return true;
        }

        switch ((lin_usb_breq_t)req->bRequest)
        {
        case LIN_USB_BREQ_BAUDRATE:
            lin_engine_configure(_ctrl_ch, &_ctrl_buf.baudrate);
            break;

        case LIN_USB_BREQ_MODE:
            /* On START, table_id selects a schedule table: reject out-of-range
             * values before the engine stores it, as for the requests below. */
            if (_ctrl_buf.mode.mode == LIN_USB_MODE_START &&
                _ctrl_buf.mode.table_id >= LIN_USB_MAX_SCHEDULE_TABLES)
            {
                return false;
            }
            lin_engine_set_mode(_ctrl_ch, &_ctrl_buf.mode);
            break;

        case LIN_USB_BREQ_FRAME_CONFIG:
            /* Reject out-of-range host-supplied fields before the engine sees
             * them: table_id indexes a table array, dlc bounds data[].
             * Returning false STALLs the status stage, so the host sees an
             * error instead of a silently dropped entry. */
            if (_ctrl_buf.schedule_entry.table_id >= LIN_USB_MAX_SCHEDULE_TABLES ||
                _ctrl_buf.schedule_entry.dlc < 1u ||
                _ctrl_buf.schedule_entry.dlc > 8u)
            {
                return false;
            }
            lin_engine_update_frame(_ctrl_ch, &_ctrl_buf.schedule_entry);
            break;

        case LIN_USB_BREQ_SCHEDULE:
        {
            /* wValue[15:8] = slot index within the table */
            uint8_t slot = (uint8_t)((req->wValue >> 8) & 0xFFu);
            /* Validate host-supplied indices/length before the engine writes
             * into schedule[table_id][slot] with them. STALL on failure so
             * the host notices (see FRAME_CONFIG). */
            if (slot >= LIN_USB_MAX_SCHEDULE_ENTRIES ||
                _ctrl_buf.schedule_entry.table_id >= LIN_USB_MAX_SCHEDULE_TABLES ||
                _ctrl_buf.schedule_entry.dlc < 1u ||
                _ctrl_buf.schedule_entry.dlc > 8u)
            {
                return false;
            }
            lin_engine_set_schedule_entry(_ctrl_ch, slot,
                                          &_ctrl_buf.schedule_entry);
            break;
        }

        case LIN_USB_BREQ_SLEEP_WAKEUP:
            lin_engine_sleep_wakeup(_ctrl_ch, _ctrl_buf.sleep_wakeup.command);
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
 * EP_OUT: host "set frame" — relay the payload to the engine.
 * EP_IN:  finished streaming a frame ("get frame") to the host.
 */
static bool linusb_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                            xfer_result_t result, uint32_t xferred_bytes)
{
    (void)rhport;

    if (ep_addr == _usb.ep_out)
    {
        if (result == XFER_RESULT_SUCCESS)
        {
            /* The host expects exactly one ACK per "set frame" packet, so a
             * rejected packet is answered with an error ACK rather than
             * silence — otherwise a host waiting on the ACK stalls. */
            bool valid = (xferred_bytes == sizeof(lin_usb_host_frame_t)) &&
                         (_out_frame.channel < LIN_USB_CHANNEL_COUNT) &&
                         (_out_frame.dlc >= 1u) && (_out_frame.dlc <= 8u);

            bool ok = valid && lin_engine_set_data(_out_frame.channel, &_out_frame);

            /* Acknowledge the update: the host cannot see otherwise whether the
             * LIN ID exists in the running schedule. The frame itself goes out
             * later, on that slot's next schedule event. */
            if (xferred_bytes == sizeof(lin_usb_host_frame_t))
            {
                /* Echo the packet back so the host can match the ACK, even when
                 * channel/dlc were out of range. */
                _ack = _out_frame;
            }
            else
            {
                /* Short packet: nothing in _out_frame can be trusted. */
                memset(&_ack, 0, sizeof(_ack));
            }
            _ack.echo_id      = LIN_USB_ECHO_ID_SET_DATA_ACK;
            _ack.timestamp_ms = HAL_GetTick();
            _ack.flags        = ok ? 0u : LIN_USB_FRAME_FLAG_ERROR;

            if (!report_frame(&_ack, false))
            {
                /* IN queue full: hold the ACK and leave the OUT endpoint
                 * un-armed; lin_usb_task() queues it and re-arms. */
                _ack_pending = true;
                return true;
            }
        }

        /* Re-arm OUT endpoint for the next "set frame" from the host; out_arm()
         * records failure so lin_usb_task() can retry. */
        out_arm();
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
const usbd_class_driver_t lin_usb_driver = {
    .name            = "LIN_USB",
    .init            = linusb_init,
    .deinit          = linusb_deinit,
    .reset           = linusb_reset,
    .open            = linusb_open,
    .control_xfer_cb = NULL,  /* vendor requests routed via tud_vendor_control_xfer_cb */
    .xfer_cb         = linusb_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void lin_usb_init(void)
{
    memset(&_usb, 0, sizeof(_usb));
    _in_head = 0;
    _in_tail = 0;
    _in_busy = false;
}

void lin_usb_task(void)
{
    /* Service the LIN engine (no-op until the real scheduler is integrated). */
    lin_engine_task();

    /* Pump any frames the engine reported toward the host.  Only touch the
     * endpoint once configured (tud_mounted): before SET_CONFIGURATION the
     * endpoint addresses are still 0 (EP0). */
    if (tud_mounted())
    {
        if (_ack_pending)
        {
            /* Queue the held set-data ACK; accept the next packet only after. */
            if (in_queue_push(&_ack, false))
            {
                _ack_pending = false;
                out_arm();
            }
        }
        /* Recover the OUT endpoint if a previous re-arm was rejected. */
        else if (!_out_armed)
        {
            out_arm();
        }
        in_try_send();
    }
}

bool lin_usb_report_frame(const lin_usb_host_frame_t *frame)
{
    return report_frame(frame, true);
}

/* -------------------------------------------------------------------------
 * Weak no-op LIN engine hooks
 *
 * The real LIN scheduler (integrated elsewhere) provides non-weak overrides
 * that own the schedule, timing, master/slave logic and the LIN bus.  Until
 * then these stubs let the USB layer build, enumerate and exchange control /
 * bulk transfers with the host while doing nothing on the bus.
 * ------------------------------------------------------------------------- */
extern void UART_SendString(USART_TypeDef *USARTx, char *str);

/*
 * Apply baud rate / timing configuration to channel `ch` (BREQ_BAUDRATE).
 * Real impl should latch every field of *cfg for use when the channel starts:
 *   - baudrate      : UART bit rate (bps), e.g. 19200.
 *   - lin_version   : LIN_USB_VERSION_* — selects classic vs enhanced checksum.
 *   - break_length  : break field length in bit times (>=13 nominal).
 *   - timebase_ms   : nominal schedule-slot floor in ms (0 = none).
 *   - slave_nad     : diagnostic node-address filter (0x7F = broadcast).
 *   - jitter_us     : extra inter-frame jitter to add between slots.
 *   - diag_*        : diagnostic transport timeouts (STmin/P2/NAS/NCR).
 * Does NOT start the bus — that happens on lin_engine_set_mode() START.
 */
__attribute__((weak)) void lin_engine_configure(uint8_t ch,
                                                const lin_usb_bus_config_t *cfg)
{
    char buf[80];
    snprintf(buf, sizeof(buf),
             "[lin] configure ch=%u baud=%lu ver=%u flags=0x%02X\r\n",
             (unsigned)ch, (unsigned long)cfg->baudrate,
             (unsigned)cfg->lin_version, (unsigned)cfg->flags);
    UART_SendString(USART1, buf);
}

/*
 * Start or stop channel `ch` and select its bus role (BREQ_MODE).
 *   - mode->mode  == LIN_USB_MODE_START      : bring the UART/LIN up and start
 *                    schedule table mode->table_id with mode->entry_count valid
 *                    entries (timing from lin_engine_configure).
 *   - mode->mode  == LIN_USB_MODE_STOP       : stop channel and schedule.
 *   - mode->mode  == LIN_USB_MODE_PAUSE: pause schedule at current slot.
 *   Master/slave role is taken from the bus config
 *   (lin_engine_configure, lin_usb_bus_config_t.flags & LIN_USB_FLAG_MASTER).
 * Once running, every frame the engine processes (published or received) MUST
 * be handed to the host via lin_usb_report_frame().
 */
__attribute__((weak)) void lin_engine_set_mode(uint8_t ch,
                                               const lin_usb_mode_t *mode)
{
    static const char * const mode_str[] = { "STOP", "START", "PAUSE" };
    const char *ms = (mode->mode < 3u) ? mode_str[mode->mode] : "?";
    char buf[64];
    snprintf(buf, sizeof(buf),
             "[lin] set_mode ch=%u mode=%s table=%u entries=%u\r\n",
             (unsigned)ch, ms,
             (unsigned)mode->table_id, (unsigned)mode->entry_count);
    UART_SendString(USART1, buf);
}

/*
 * Install one schedule-table entry at `slot` (BREQ_SCHEDULE).
 * *entry describes the slot (see lin_usb_schedule_entry_t):
 *   - table_id   : which table the entry belongs to (0 .. MAX_TABLES-1).
 *   - lin_id     : protected LIN ID for this slot.
 *   - direction  : 0 = publisher (device sends entry->data),
 *                  1 = subscriber (device sends header, slave replies).
 *   - dlc        : payload length 1..8.
 *   - flags      : LIN_USB_FRAME_FLAG_* (e.g. ENHANCED_CS, SPORADIC).
 *   - period_ms  : nominal slot period.
 *   - data[8]    : initial TX payload for publisher slots.
 * Real impl stores it into schedule[table_id][slot].
 */
__attribute__((weak)) void lin_engine_set_schedule_entry(uint8_t ch, uint8_t slot,
                                       const lin_usb_schedule_entry_t *entry)
{
    char buf[80];
    snprintf(buf, sizeof(buf),
             "[lin] set_entry ch=%u slot=%u table=%u id=0x%02X dlc=%u period=%u\r\n",
             (unsigned)ch, (unsigned)slot, (unsigned)entry->table_id,
             (unsigned)entry->lin_id, (unsigned)entry->dlc,
             (unsigned)entry->period_ms);
    UART_SendString(USART1, buf);
}

/*
 * Update an already-installed entry, matched by entry->lin_id within
 * entry->table_id (BREQ_FRAME_CONFIG).  Use this to change a slot's
 * parameters/payload without re-uploading by slot index.  If no entry with
 * that LIN ID exists, do nothing.
 */
__attribute__((weak)) void lin_engine_update_frame(uint8_t ch,
                                       const lin_usb_schedule_entry_t *entry)
{
    char buf[64];
    snprintf(buf, sizeof(buf),
             "[lin] update_frame ch=%u table=%u id=0x%02X dlc=%u\r\n",
             (unsigned)ch, (unsigned)entry->table_id,
             (unsigned)entry->lin_id, (unsigned)entry->dlc);
    UART_SendString(USART1, buf);
}

/*
 * Bus sleep / wakeup (BREQ_SLEEP_WAKEUP).
 *   - command == 0 : go-to-sleep — send the LIN sleep command (or let the bus
 *                    idle), stop the schedule.
 *   - command == 1 : wakeup — drive a wakeup pulse and resume the schedule.
 * Real impl should also report the event to the host via lin_usb_report_frame()
 * with LIN_USB_FRAME_FLAG_SLEEP / _TX_UPDATE set, as appropriate.
 */
__attribute__((weak)) void lin_engine_sleep_wakeup(uint8_t ch, uint8_t command)
{
    char buf[48];
    snprintf(buf, sizeof(buf),
             "[lin] sleep_wakeup ch=%u cmd=%u\r\n",
             (unsigned)ch, (unsigned)command);
    UART_SendString(USART1, buf);
}

/*
 * "Set frame" (bulk OUT): update the TX payload that the publisher slot whose
 * LIN ID equals frame->lin_id will send on its next turn.  Copy frame->dlc
 * bytes from frame->data into the matching entry in the active table.  A LIN
 * ID that is not a publisher in the running schedule is a harmless no-op.
 * This does NOT transmit immediately — the self-scheduling engine sends the
 * new data when that slot next comes around.
 */
__attribute__((weak)) bool lin_engine_set_data(uint8_t ch,
                                               const lin_usb_host_frame_t *frame)
{
    char buf[64];
    snprintf(buf, sizeof(buf),
             "[lin] set_data ch=%u id=0x%02X dlc=%u\r\n",
             (unsigned)ch, (unsigned)frame->lin_id, (unsigned)frame->dlc);
    UART_SendString(USART1, buf);
    return true;
}

/*
 * Visual identify for channel `ch` (BREQ_IDENTIFY): flash an LED or similar so
 * the user can locate the adapter.  May be a no-op if there is no indicator.
 */
__attribute__((weak)) void lin_engine_get_bus_state(uint8_t ch,
                                                    lin_usb_bus_state_t *state)
{
    (void)ch;
    state->state = LIN_USB_BUS_STATE_OK;
}

__attribute__((weak)) void lin_engine_identify(uint8_t ch)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "[lin] identify ch=%u\r\n", (unsigned)ch);
    UART_SendString(USART1, buf);
}

/*
 * Periodic service, called every lin_usb_task() iteration (main loop).
 * If the engine is polled (no timer/ISR), advance the schedule here: when a
 * slot is due, run it on the bus and call lin_usb_report_frame() for the
 * resulting frame (TX echo or subscriber RX).  Also service slave-mode RX.
 * If the engine runs off its own timer/interrupts, this can stay empty.
 */
__attribute__((weak)) void lin_engine_task(void)
{
}

/*
 * USB bus reset: stop every channel the host had started.
 */
__attribute__((weak)) void lin_engine_reset(void)
{
}
