#include "LindeApi.hpp"
#include "CandeApi.hpp"
#include "AiodeApi.hpp"
#include "lin_usb_protocol.h"
#include "gs_usb_protocol.h"
#include "aio_usb_protocol.h"

#include <iostream>
#include <iomanip>
#include <cstring>
#include <thread>
#include <chrono>

/* =========================================================================
 * Generic helpers
 * ========================================================================= */

static void printBytes(const uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++)
    {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(data[i]);
        if (i + 1 < len)
        {
            std::cout << ' ';
        }
    }
    std::cout << std::dec;
}

static unsigned g_errors = 0;

static bool check(bool ok, const std::string &err, const char *what)
{
    if (!ok)
    {
        std::cerr << "[ERROR] " << what << " — " << err << "\n";
        ++g_errors;
    }
    return ok;
}

/* =========================================================================
 * LIN section
 * ========================================================================= */

static void printLinDeviceConfig(const lin_usb_device_config_t &cfg)
{
    std::cout << "[LIN device config]\n";
    std::cout << "  channels        : " << static_cast<unsigned>(cfg.icount + 1u) << "\n";
    std::cout << "  schedule tables : " << static_cast<unsigned>(cfg.schedule_tables)
              << " x " << static_cast<unsigned>(cfg.schedule_entries) << " entries\n";
    std::cout << "  sw version      : 0x" << std::hex << cfg.sw_version << std::dec << "\n";
    std::cout << "  hw version      : 0x" << std::hex << cfg.hw_version << std::dec << "\n";

    std::cout << "  features        :";
    if (cfg.features & LIN_USB_FEATURE_SCHEDULING)      std::cout << " SCHEDULING";
    if (cfg.features & LIN_USB_FEATURE_TIMESTAMP)       std::cout << " TIMESTAMP";
    if (cfg.features & LIN_USB_FEATURE_CUSTOM_BAUDRATE) std::cout << " CUSTOM_BAUD";
    if (cfg.features & LIN_USB_FEATURE_BUS_STATE)       std::cout << " BUS_STATE";
    if (cfg.features & LIN_USB_FEATURE_LISTEN_ONLY)     std::cout << " LISTEN_ONLY";
    std::cout << "\n";

    std::cout << "  baudrates       :";
    if (cfg.supported_baudrates & LIN_USB_BAUD_1200)  std::cout << " 1200";
    if (cfg.supported_baudrates & LIN_USB_BAUD_2400)  std::cout << " 2400";
    if (cfg.supported_baudrates & LIN_USB_BAUD_4800)  std::cout << " 4800";
    if (cfg.supported_baudrates & LIN_USB_BAUD_9600)  std::cout << " 9600";
    if (cfg.supported_baudrates & LIN_USB_BAUD_10417) std::cout << " 10417";
    if (cfg.supported_baudrates & LIN_USB_BAUD_19200) std::cout << " 19200";
    if (cfg.supported_baudrates & LIN_USB_BAUD_20000) std::cout << " 20000";
    std::cout << "\n";
}

static void printLinFrame(const char *tag, const lin_usb_host_frame_t &f)
{
    std::cout << "[LIN " << tag << "] "
              /* lin_id from the device is the protected ID (ID + parity bits) */
              << "id=0x"  << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(f.lin_id & 0x3Fu)
              << " pid=0x" << std::setw(2) << static_cast<unsigned>(f.lin_id)
              << " ch="   << std::dec << static_cast<unsigned>(f.channel)
              << " dlc="  << static_cast<unsigned>(f.dlc)
              << " flags=0x" << std::hex << std::setw(2) << std::setfill('0')
                             << static_cast<unsigned>(f.flags)
              << " ts="   << std::dec << f.timestamp_ms << "ms"
              << " data=[";
    printBytes(f.data, f.dlc);
    std::cout << "]";
    if (f.flags & LIN_USB_FRAME_FLAG_ERROR)    std::cout << " ERROR";
    if (f.flags & LIN_USB_FRAME_FLAG_SLEEP)    std::cout << " SLEEP";
    if (f.flags & LIN_USB_FRAME_FLAG_WAKEUP)   std::cout << " WAKEUP";
    if (f.echo_id == LIN_USB_ECHO_ID_RX)       std::cout << " (bus event)";
    else if (f.echo_id == LIN_USB_ECHO_ID_SET_DATA_ACK) std::cout << " (set-data ACK)";
    else                                        std::cout << " (echo#" << f.echo_id << ")";
    std::cout << "\n";
}

static void sampleLinSchedule(LindeApi &api)
{
    std::cout << "\n--- LIN schedule test (table 0, 2 entries, 10 ms slot) ---\n";

    /* Entry 0: publisher (device sends data we set).  Entry 1: subscriber. */
    lin_usb_schedule_entry_t e0{};
    e0.lin_id = 0x21u; e0.direction = 0u; e0.dlc = 4u;
    e0.flags = LIN_USB_FRAME_FLAG_ENHANCED_CS; e0.period_ms = 20u;
    e0.data[0] = 0x01; e0.data[1] = 0x02; e0.data[2] = 0x03; e0.data[3] = 0x04;

    lin_usb_schedule_entry_t e1{};
    e1.lin_id = 0x22u; e1.direction = 1u; e1.dlc = 4u;
    e1.flags = LIN_USB_FRAME_FLAG_ENHANCED_CS; e1.period_ms = 20u;

    if (!check(api.uploadScheduleEntry(0, 0, e0), api.lastError(), "upload entry 0")) return;
    if (!check(api.uploadScheduleEntry(0, 1, e1), api.lastError(), "upload entry 1")) return;
    if (!check(api.scheduleStart(0, 2),            api.lastError(), "scheduleStart"))  return;

    std::cout << "collecting 6 scheduled frames (getFrame)...\n";
    for (int i = 0; i < 6; i++)
    {
        lin_usb_host_frame_t f{};
        if (api.getFrame(f, 500u))
        {
            printLinFrame("sched", f);
        }
        else
        {
            std::cout << "[LIN] timeout (" << api.lastError() << ")\n";
            break;
        }
    }

    /* setFrame: change the publisher entry's payload while the schedule runs.
     * The device acknowledges the update (setFrame() waits for that ACK) and
     * sends the new data on entry 0x21's next slot. */
    std::cout << "setFrame: updating publisher 0x21 payload to CA FE C0 DE...\n";
    lin_usb_host_frame_t upd{};
    upd.channel = 0u;
    upd.lin_id  = 0x21u;   /* must match the scheduled publisher entry */
    upd.dlc     = 4u;
    upd.data[0] = 0xCA; upd.data[1] = 0xFE; upd.data[2] = 0xC0; upd.data[3] = 0xDE;
    if (check(api.setFrame(upd), api.lastError(), "LIN setFrame"))
    {
        std::cout << "setFrame acknowledged by device.\n";
    }

    /* A LIN ID that is not in the running schedule is rejected in the ACK. */
    lin_usb_host_frame_t bad = upd;
    bad.lin_id = 0x3Au;
    if (api.setFrame(bad))
    {
        std::cout << "[LIN] setFrame for unscheduled id 0x3A was accepted "
                     "(stub engine accepts everything).\n";
    }
    else
    {
        std::cout << "[LIN] setFrame for unscheduled id 0x3A rejected as expected ("
                  << api.lastError() << ").\n";
    }

    std::cout << "collecting 6 more frames (expect 0x21 to carry new payload)...\n";
    for (int i = 0; i < 6; i++)
    {
        lin_usb_host_frame_t f{};
        if (api.getFrame(f, 500u))
        {
            printLinFrame("sched", f);
        }
        else
        {
            std::cout << "[LIN] timeout (" << api.lastError() << ")\n";
            break;
        }
    }

    check(api.scheduleStop(), api.lastError(), "scheduleStop");
}

/* Out-of-range control requests are STALLed by the device, so the host sees an
 * error instead of a silently dropped entry. */
static void sampleLinRejects(LindeApi &api, const lin_usb_device_config_t &cfg)
{
    std::cout << "\n--- LIN invalid-request test (expect STALL) ---\n";

    lin_usb_schedule_entry_t e{};
    e.lin_id = 0x21u; e.dlc = 4u; e.period_ms = 20u;

    /* slot == schedule_entries is one past the last valid slot */
    if (api.uploadScheduleEntry(0, cfg.schedule_entries, e))
    {
        check(false, "device accepted an out-of-range slot", "STALL on bad slot");
    }
    else
    {
        std::cout << "bad slot rejected: " << api.lastError() << "\n";
    }

    e.dlc = 9u;   /* LIN frames carry 1..8 bytes */
    if (api.uploadScheduleEntry(0, 0, e))
    {
        check(false, "device accepted dlc 9", "STALL on bad dlc");
    }
    else
    {
        std::cout << "bad dlc rejected: " << api.lastError() << "\n";
    }
}

static void sampleLinSleepWakeup(LindeApi &api)
{
    std::cout << "\n--- LIN sleep/wakeup test ---\n";

    if (!check(api.goToSleep(), api.lastError(), "goToSleep")) return;
    std::cout << "sleep sent.\n";

    lin_usb_host_frame_t f{};
    if (api.getFrame(f, 200u)) printLinFrame("sleep-event", f);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!check(api.wakeup(), api.lastError(), "wakeup")) return;
    std::cout << "wakeup sent.\n";

    if (api.getFrame(f, 200u)) printLinFrame("wakeup-event", f);
}

/* =========================================================================
 * CAN section
 * ========================================================================= */

static void printCanDeviceConfig(const gs_device_config_t &cfg,
                                  const gs_device_bt_const_ext_t &ext)
{
    const gs_device_bt_const_t &bt = ext.nominal;
    std::cout << "[CAN device config]\n";
    std::cout << "  channels   : " << static_cast<unsigned>(cfg.icount + 1u) << "\n";
    std::cout << "  sw version : 0x" << std::hex << cfg.sw_version << std::dec << "\n";
    std::cout << "  hw version : 0x" << std::hex << cfg.hw_version << std::dec << "\n";
    std::cout << "  fclk_can   : " << bt.fclk_can << " Hz\n";
    std::cout << "  tseg1      : " << bt.tseg1_min << ".." << bt.tseg1_max << " TQ\n";
    std::cout << "  tseg2      : " << bt.tseg2_min << ".." << bt.tseg2_max << " TQ\n";
    std::cout << "  brp        : " << bt.brp_min   << ".." << bt.brp_max
              << " (inc " << bt.brp_inc << ")\n";

    std::cout << "  features   :";
    if (bt.feature & GS_CAN_FEATURE_LISTEN_ONLY)   std::cout << " LISTEN_ONLY";
    if (bt.feature & GS_CAN_FEATURE_LOOP_BACK)     std::cout << " LOOP_BACK";
    if (bt.feature & GS_CAN_FEATURE_ONE_SHOT)      std::cout << " ONE_SHOT";
    if (bt.feature & GS_CAN_FEATURE_HW_TIMESTAMP)  std::cout << " HW_TIMESTAMP";
    if (bt.feature & GS_CAN_FEATURE_IDENTIFY)      std::cout << " IDENTIFY";
    if (bt.feature & GS_CAN_FEATURE_FD)            std::cout << " FD";
    if (bt.feature & GS_CAN_FEATURE_BT_CONST_EXT)  std::cout << " BT_CONST_EXT";
    if (bt.feature & GS_CAN_FEATURE_GET_STATE)     std::cout << " GET_STATE";
    if (bt.feature & GS_CAN_FEATURE_AUTO_RESTART)  std::cout << " AUTO_RESTART";
    std::cout << "\n";

    if (bt.feature & GS_CAN_FEATURE_BT_CONST_EXT)
    {
        std::cout << "  FD dtseg1  : " << ext.dtseg1_min << ".." << ext.dtseg1_max << " TQ\n";
        std::cout << "  FD dtseg2  : " << ext.dtseg2_min << ".." << ext.dtseg2_max << " TQ\n";
        std::cout << "  FD dbrp    : " << ext.dbrp_min   << ".." << ext.dbrp_max
                  << " (inc " << ext.dbrp_inc << ")\n";
    }
}

static void printCanState(CandeApi &can, uint8_t ch)
{
    static const char *const names[] = {
        "ERROR_ACTIVE", "ERROR_WARNING", "ERROR_PASSIVE", "BUS_OFF", "STOPPED", "SLEEPING"
    };
    gs_device_state_t st{};
    if (check(can.getState(ch, st), can.lastError(), "getState"))
    {
        std::cout << "[CAN state] ch=" << static_cast<unsigned>(ch)
                  << " state=" << (st.state < 6u ? names[st.state] : "UNKNOWN")
                  << " rxerr=" << st.rxerr << " txerr=" << st.txerr << "\n";
    }
}

static void printCanFrame(const char *tag, const gs_host_frame_t &f)
{
    bool eff = (f.can_id & GS_CAN_EFF_FLAG) != 0;
    bool rtr = (f.can_id & GS_CAN_RTR_FLAG) != 0;
    bool err = (f.can_id & GS_CAN_ERR_FLAG) != 0;
    uint32_t id = f.can_id & (eff ? GS_CAN_EFF_MASK : GS_CAN_SFF_MASK);

    bool fd  = (f.flags & GS_FRAME_FLAG_FD) != 0u;
    /* FD: can_dlc is a DLC code; classic: a byte count 0..8 */
    uint8_t len = fd ? gs_dlc_to_len(f.can_dlc) : (f.can_dlc > 8u ? 8u : f.can_dlc);

    std::cout << "[CAN " << tag << "] "
              << (fd ? "FD " : "") << (eff ? "EXT" : "STD")
              << " id=0x" << std::hex << std::setw(eff ? 8 : 3) << std::setfill('0') << id
              << " ch=" << std::dec << static_cast<unsigned>(f.channel)
              << " len=" << static_cast<unsigned>(len)
              << " data=[";
    printBytes(fd ? f.fd.data : f.classic.data, len);
    std::cout << "]";
    if (f.flags & GS_FRAME_FLAG_BRS) std::cout << " BRS";
    if (rtr) std::cout << " RTR";
    if (err) std::cout << " ERR";
    uint32_t ts = fd ? f.fd.timestamp_us : f.classic.timestamp_us;
    if (ts != 0u) std::cout << " ts=" << ts << "us";
    if (f.echo_id == GS_ECHO_ID_RX) std::cout << " (RX)";
    else                             std::cout << " (echo#" << f.echo_id << ")";
    std::cout << "\n";
}

/* Bit timing for `bitrate` from the device's clock and limits: the smallest
 * prescaler that divides the clock exactly into at most 25 time quanta, sample
 * point ~87.5 % (nominal) or ~75 % (data phase).  The gs_usb engine adds
 * prop_seg + phase_seg1, so the split between the two does not matter. */
static bool calcBitTiming(uint32_t fclk, uint32_t bitrate, bool data_phase,
                          uint32_t brp_min, uint32_t brp_max,
                          uint32_t tseg1_max, uint32_t tseg2_max, uint32_t sjw_max,
                          gs_device_bittiming_t &bt)
{
    for (uint32_t brp = brp_min ? brp_min : 1u; brp <= brp_max; brp++)
    {
        if (fclk % (brp * bitrate) != 0u)
        {
            continue;
        }
        uint32_t tq = fclk / (brp * bitrate);
        if (tq > 25u || tq < 8u)
        {
            continue;
        }
        uint32_t tseg2 = data_phase ? tq / 4u : tq / 8u;
        if (tseg2 < 2u) tseg2 = 2u;
        uint32_t tseg1 = tq - 1u - tseg2;
        if (tseg1 > tseg1_max || tseg2 > tseg2_max)
        {
            continue;
        }
        bt.brp        = brp;
        bt.prop_seg   = tseg1 / 2u;
        bt.phase_seg1 = tseg1 - bt.prop_seg;
        bt.phase_seg2 = tseg2;
        bt.sjw        = tseg2 < sjw_max ? tseg2 : sjw_max;
        return true;
    }
    return false;
}

/* Wait for the TX echo with `echo_id`, printing anything else that arrives
 * (error frames, frames received on other channels) on the way. */
static bool waitEcho(CandeApi &can, uint32_t echo_id, const char *tag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline)
    {
        gs_host_frame_t rx{};
        if (!can.receiveFrame(rx, 50u))
        {
            continue;
        }
        if (rx.echo_id == echo_id)
        {
            printCanFrame(tag, rx);
            return true;
        }
        printCanFrame((rx.can_id & GS_CAN_ERR_FLAG) ? "error" : "rx", rx);
    }
    std::cout << "[CAN] no echo within 200 ms -- no other node ACKed the frame "
                 "(bus not connected / no partner)\n";
    return false;
}

static void sampleCanFrames(CandeApi &can)
{
    std::cout << "\n--- CAN single frame test ---\n";

    /* Standard 11-bit frame */
    gs_host_frame_t tx{};
    tx.echo_id  = 1u;
    tx.can_id   = 0x123u; /* 11-bit ID, no flags */
    tx.can_dlc  = 4u;
    tx.channel  = 0u;
    tx.classic.data[0] = 0xCA; tx.classic.data[1] = 0xFE;
    tx.classic.data[2] = 0xBA; tx.classic.data[3] = 0xBE;

    printCanFrame("send-std", tx);
    if (!check(can.sendFrame(tx), can.lastError(), "CAN sendFrame STD"))
    {
        return;
    }

    waitEcho(can, tx.echo_id, "echo-std");

    /* Extended 29-bit frame */
    std::memset(&tx, 0, sizeof(tx));
    tx.echo_id = 2u;
    tx.can_id  = 0x1FEDCBA9u | GS_CAN_EFF_FLAG;
    tx.can_dlc = 8u;
    tx.channel = 0u;
    for (uint8_t i = 0; i < 8u; i++) tx.classic.data[i] = i;

    printCanFrame("send-ext", tx);
    if (!check(can.sendFrame(tx), can.lastError(), "CAN sendFrame EXT"))
    {
        return;
    }

    waitEcho(can, tx.echo_id, "echo-ext");
}

/* CAN FD: 2 Mbit/s data phase with bit-rate switch, 16-byte payload. */
static void sampleCanFd(CandeApi &can, const gs_device_bt_const_ext_t &ext,
                        uint8_t nch, uint32_t base_flags)
{
    std::cout << "\n--- CAN FD test (500k / 2M, BRS) ---\n";

    gs_device_bittiming_t dt{};
    if (!check(calcBitTiming(ext.nominal.fclk_can, 2000000u, true, ext.dbrp_min, ext.dbrp_max,
                             ext.dtseg1_max, ext.dtseg2_max, ext.dsjw_max, dt),
               "no exact 2 Mbit/s timing for this clock", "calc data bit timing"))
    {
        return;
    }
    std::cout << "data phase 2 Mbit/s: brp=" << dt.brp << " tseg1=" << dt.prop_seg + dt.phase_seg1
              << " tseg2=" << dt.phase_seg2 << "\n";

    /* Bit timing is applied on START, so restart the channel(s) in FD mode. */
    for (uint8_t ch = 0; ch < nch; ch++)
    {
        if (!check(can.setMode(ch, GS_CAN_MODE_RESET, 0u), can.lastError(), "setMode RESET") ||
            !check(can.setDataBitTiming(ch, dt), can.lastError(), "setDataBitTiming") ||
            !check(can.setMode(ch, GS_CAN_MODE_START, base_flags | GS_CAN_FLAG_FD),
                   can.lastError(), "setMode START FD"))
        {
            return;
        }
    }

    gs_host_frame_t tx{};
    tx.echo_id = 4u;
    tx.can_id  = 0x456u;
    tx.flags   = GS_FRAME_FLAG_FD | GS_FRAME_FLAG_BRS;
    tx.can_dlc = gs_len_to_dlc(16u);   /* DLC code 10 */
    tx.channel = 0u;
    for (uint8_t i = 0; i < 16u; i++) tx.fd.data[i] = static_cast<uint8_t>(0xF0u + i);

    printCanFrame("send-fd", tx);
    if (!check(can.sendFrame(tx), can.lastError(), "CAN sendFrame FD"))
    {
        return;
    }

    waitEcho(can, tx.echo_id, "echo-fd");

    /* Back to classic mode for the remaining tests */
    for (uint8_t ch = 0; ch < nch; ch++)
    {
        can.setMode(ch, GS_CAN_MODE_RESET, 0u);
        can.setMode(ch, GS_CAN_MODE_START, base_flags);
    }
}

static void sampleCanLoopback(CandeApi &can, uint32_t base_flags)
{
    std::cout << "\n--- CAN loopback test ---\n";

    /* Restart with loopback flag so the device echoes TX back as RX */
    if (!check(can.setMode(0, GS_CAN_MODE_START, base_flags | GS_CAN_FLAG_LOOP_BACK),
               can.lastError(), "setMode LOOPBACK"))
    {
        return;
    }

    gs_host_frame_t tx{};
    tx.echo_id = 3u;
    tx.can_id  = 0x7FFu; /* highest 11-bit ID */
    tx.can_dlc = 2u;
    tx.channel = 0u;
    tx.classic.data[0] = 0xAB; tx.classic.data[1] = 0xCD;

    printCanFrame("send", tx);
    if (!check(can.sendFrame(tx), can.lastError(), "CAN sendFrame loopback"))
    {
        return;
    }

    /* Expect TX echo + loopback RX (two frames) */
    for (int i = 0; i < 2; i++)
    {
        gs_host_frame_t rx{};
        if (can.receiveFrame(rx, 200u))
        {
            printCanFrame(rx.echo_id == GS_ECHO_ID_RX ? "lb-rx" : "lb-echo", rx);
        }
        else
        {
            std::cout << "[CAN] timeout (" << can.lastError() << ")\n";
            break;
        }
    }

    /* Restore normal mode */
    can.setMode(0, GS_CAN_MODE_START, base_flags);
}

/* =========================================================================
 * AIO section
 * ========================================================================= */

static void printAioCaps(const aio_usb_caps_t &caps)
{
    std::cout << "[AIO device config]\n";
    std::cout << "  digital I/O     : " << static_cast<unsigned>(caps.io_count) << "\n";
    std::cout << "  analog channels : " << static_cast<unsigned>(caps.analog_count) << "\n";
    std::cout << "  analog resol.   : " << static_cast<unsigned>(caps.analog_resolution_bits)
              << " bit\n";
    std::cout << "  input-capable   : 0x" << std::hex << caps.io_input_capable_mask
              << std::dec << "\n";
    std::cout << "  output-capable  : 0x" << std::hex << caps.io_output_capable_mask
              << std::dec << "\n";
    std::cout << "  sw version      : 0x" << std::hex << caps.sw_version << std::dec << "\n";
    std::cout << "  hw version      : 0x" << std::hex << caps.hw_version << std::dec << "\n";

    std::cout << "  features        :";
    if (caps.features & AIO_USB_FEATURE_TIMESTAMP)   std::cout << " TIMESTAMP";
    if (caps.features & AIO_USB_FEATURE_AUTO_REPORT) std::cout << " AUTO_REPORT";
    if (caps.features & AIO_USB_FEATURE_ANALOG)      std::cout << " ANALOG";
    std::cout << "\n";
}

static void printAioStatus(const char *tag, const aio_usb_report_t &st,
                            uint8_t analog_count)
{
    std::cout << "[AIO " << tag << "] ts=" << st.timestamp_ms << "ms"
              << " io=0x"  << std::hex << std::setw(8) << std::setfill('0') << st.io_states
              << " dir=0x" << std::setw(8) << std::setfill('0') << st.io_direction
              << std::dec << std::setfill(' ') << "\n";
    std::cout << "  analog:";
    for (uint8_t i = 0; i < analog_count && i < AIO_USB_ANALOG_COUNT; i++)
    {
        std::cout << ' ' << st.analog[i];
    }
    std::cout << "\n";
}

static void sampleAio(AiodeApi &api, const aio_usb_caps_t &caps)
{
    std::cout << "\n--- AIO config + read test ---\n";

    /* Configure line 0 as output (default low), line 1 as input. */
    check(api.configIo(0, AIO_USB_IO_MODE_OUTPUT, /*default*/0, /*flags*/0, /*report*/0),
          api.lastError(), "configIo out 0");
    check(api.configIo(1, AIO_USB_IO_MODE_INPUT, 0,
                       AIO_USB_IO_FLAG_PULLUP, 0),
          api.lastError(), "configIo in 1");

    /* Drive line 0 high, then read back a snapshot. */
    if (check(api.setOutputs(0x1u, 0x1u), api.lastError(), "setOutputs line0=1"))
    {
        std::cout << "line 0 driven high.\n";
    }

    aio_usb_report_t st{};
    if (check(api.readStatus(st), api.lastError(), "readStatus"))
    {
        printAioStatus("status", st, caps.analog_count);
    }

    /* Enable a 50 ms auto-report on line 0 and collect a few frames. */
    std::cout << "\n--- AIO auto-report test (50 ms) ---\n";
    if (caps.features & AIO_USB_FEATURE_AUTO_REPORT)
    {
        check(api.configIo(0, AIO_USB_IO_MODE_OUTPUT, 1, 0, /*report*/50),
              api.lastError(), "configIo auto-report 50ms");

        for (int i = 0; i < 4; i++)
        {
            aio_usb_report_t f{};
            if (!check(api.receiveReport(f, 500u), api.lastError(), "AIO receiveReport"))
            {
                break;
            }
            printAioStatus("report", f, caps.analog_count);
        }

        /* Disable auto-report again. */
        check(api.configIo(0, AIO_USB_IO_MODE_OUTPUT, 1, 0, /*report*/0),
              api.lastError(), "configIo auto-report off");
    }
    else
    {
        std::cout << "[AIO] AUTO_REPORT not advertised -- skipping.\n";
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main()
{
    std::cout << "CanLinAioSample -- CAN + LIN + AIO USB adapter test\n";
    std::cout << "(CandeApi / LindeApi / AiodeApi)\n";
    std::cout << "====================================================================\n";

    /* LIN, CAN and AIO live on separate USB interfaces (different
     * bInterfaceProtocol values and endpoint addresses), so all handles can be
     * open simultaneously on the same physical device. */
    LindeApi lin(/* channel = */ 0);
    CandeApi can;
    AiodeApi aio;

    std::cout << "Opening LIN interface...\n";
    const bool lin_ok = lin.open();
    if (!lin_ok)
    {
        std::cerr << "[WARN] LIN not available (" << lin.lastError() << ") -- skipping.\n";
    }
    else
    {
        std::cout << "LIN opened (ch 0 of "
                  << static_cast<unsigned>(lin.channelCount()) << ").\n";
    }

    std::cout << "Opening CAN interface...\n";
    const bool can_ok = can.open();
    if (!can_ok)
    {
        std::cerr << "[WARN] CAN not available (" << can.lastError() << ") -- skipping.\n";
    }
    else
    {
        std::cout << "CAN opened ("
                  << static_cast<unsigned>(can.channelCount()) << " channel(s)).\n";
    }

    std::cout << "Opening AIO interface...\n";
    const bool aio_ok = aio.open();
    if (!aio_ok)
    {
        std::cerr << "[WARN] AIO not available (" << aio.lastError() << ") -- skipping.\n";
    }
    else
    {
        std::cout << "AIO opened.\n";
    }

    /* ------------------------------------------------------------------ */
    /*  LIN tests (interface stays open while CAN tests run after)         */
    /* ------------------------------------------------------------------ */
    const unsigned lin_errors_before = g_errors;
    if (lin_ok)
    {
        std::cout << "\n=== LIN (lin_usb) ===\n";

        check(lin.setHostFormat(), lin.lastError(), "setHostFormat");

        lin_usb_device_config_t lcfg{};
        if (check(lin.getDeviceConfig(lcfg), lin.lastError(), "getDeviceConfig"))
        {
            printLinDeviceConfig(lcfg);
        }

        uint32_t ts = 0;
        if (lin.getTimestamp(ts))
        {
            std::cout << "[LIN timestamp] " << ts << " ms\n";
        }

        if (lcfg.features & LIN_USB_FEATURE_BUS_STATE)
        {
            lin_usb_bus_state_t bstate{};
            if (check(lin.getBusState(bstate), lin.lastError(), "getBusState"))
            {
                static const char *const state_names[] = {
                    "OK", "BUS_OFF", "PASSIVE", "ERROR", "STOPPED", "SLEEPING"
                };
                const char *name = (bstate.state < 6u)
                                 ? state_names[bstate.state] : "UNKNOWN";
                std::cout << "[LIN bus state] ch=" << static_cast<unsigned>(0)
                          << " state=" << name
                          << " (" << static_cast<unsigned>(bstate.state) << ")"
                          << " dropped=" << bstate.dropped << "\n";
            }
        }
        else
        {
            std::cout << "[LIN] BUS_STATE not advertised -- skipping.\n";
        }

        lin_usb_bus_config_t baud{};
        baud.baudrate     = 19200u;
        baud.lin_version  = LIN_USB_VERSION_2X;
        baud.break_length = 13u;
        baud.slave_nad    = 0x7Fu;
        baud.timebase_ms  = 10u;
        baud.flags        = LIN_USB_FLAG_MASTER;

        if (check(lin.setBusConfig(baud), lin.lastError(), "setBusConfig"))
        {
            std::cout << "LIN baudrate set to 19200 bps.\n";
        }

        if (check(lin.setMode(LIN_USB_MODE_START), lin.lastError(), "setMode START"))
        {
            std::cout << "LIN channel started (master).\n";
        }

        lin.identify();

        if (lcfg.features & LIN_USB_FEATURE_SCHEDULING)
        {
            sampleLinSchedule(lin);
        }
        else
        {
            std::cout << "[LIN] SCHEDULING not advertised -- skipping schedule test.\n";
        }

        sampleLinRejects(lin, lcfg);

        sampleLinSleepWakeup(lin);

        check(lin.setMode(LIN_USB_MODE_STOP), lin.lastError(), "setMode STOP");
        std::cout << "LIN channel stopped.\n";
    }

    const unsigned lin_errors = g_errors - lin_errors_before;

    /* ------------------------------------------------------------------ */
    /*  AIO tests (LIN + CAN interfaces still open in parallel)            */
    /* ------------------------------------------------------------------ */
    const unsigned aio_errors_before = g_errors;
    if (aio_ok)
    {
        std::cout << "\n=== AIO (aio_usb) ===\n";

        check(aio.setHostFormat(), aio.lastError(), "setHostFormat");

        aio_usb_caps_t caps{};
        if (check(aio.getCaps(caps), aio.lastError(), "getCaps"))
        {
            printAioCaps(caps);
        }

        uint32_t ts = 0;
        if (aio.getTimestamp(ts))
        {
            std::cout << "[AIO timestamp] " << ts << " ms\n";
        }

        aio.identify();

        sampleAio(aio, caps);
    }

    const unsigned aio_errors = g_errors - aio_errors_before;

    /* ------------------------------------------------------------------ */
    /*  CAN tests (LIN interface is still open in parallel)                */
    /* ------------------------------------------------------------------ */
    const unsigned can_errors_before = g_errors;
    if (can_ok)
    {
        std::cout << "\n=== CAN (gs_usb) ===\n";

        check(can.setHostFormat(), can.lastError(), "setHostFormat");

        gs_device_config_t       ccfg{};
        gs_device_bt_const_ext_t ext{};
        gs_device_bt_const_t    &bt = ext.nominal;
        check(can.getDeviceConfig(ccfg),    can.lastError(), "getDeviceConfig");
        check(can.getBitTimingConst(0, bt), can.lastError(), "getBitTimingConst");
        if (bt.feature & GS_CAN_FEATURE_BT_CONST_EXT)
        {
            check(can.getBitTimingConstExt(0, ext), can.lastError(), "getBitTimingConstExt");
        }
        printCanDeviceConfig(ccfg, ext);

        uint32_t ts = 0;
        if (can.getTimestamp(ts))
        {
            std::cout << "[CAN timestamp] " << ts << " us\n";
        }

        /* Ask for per-frame timestamps when the device offers them. */
        const uint32_t base_flags = (bt.feature & GS_CAN_FEATURE_HW_TIMESTAMP)
                                  ? GS_CAN_FLAG_HW_TIMESTAMP : 0u;

        /* 500 kbit/s, computed from the clock the device reports. */
        gs_device_bittiming_t timing{};
        if (check(calcBitTiming(bt.fclk_can, 500000u, false, bt.brp_min, bt.brp_max,
                                bt.tseg1_max, bt.tseg2_max, bt.sjw_max, timing),
                  "no exact 500 kbit/s timing for this clock", "calc bit timing"))
        {
            std::cout << "500 kbit/s @ " << bt.fclk_can << " Hz: brp=" << timing.brp
                      << " tseg1=" << timing.prop_seg + timing.phase_seg1
                      << " tseg2=" << timing.phase_seg2 << "\n";
        }

        /* Start every channel (up to 2): if two channels share a bus, the
         * second one ACKs what the first sends, so echoes come back and the
         * frames show up as RX on the other channel. */
        const uint8_t nch = can.channelCount() < 2u ? can.channelCount() : 2u;
        for (uint8_t ch = 0; ch < nch; ch++)
        {
            check(can.setBitTiming(ch, timing), can.lastError(), "setBitTiming");
            if (check(can.setMode(ch, GS_CAN_MODE_START, base_flags), can.lastError(), "setMode START"))
            {
                std::cout << "CAN channel " << static_cast<unsigned>(ch) << " started.\n";
            }
        }

        if (bt.feature & GS_CAN_FEATURE_GET_STATE)
        {
            printCanState(can, 0);
        }

        can.identify(0);

        sampleCanFrames(can);

        /* The FD data-phase limits come from BT_CONST_EXT. */
        if ((bt.feature & GS_CAN_FEATURE_FD) && (bt.feature & GS_CAN_FEATURE_BT_CONST_EXT))
        {
            sampleCanFd(can, ext, nch, base_flags);
        }
        else
        {
            std::cout << "[CAN] FD not advertised -- skipping CAN FD test.\n";
        }

        if (bt.feature & GS_CAN_FEATURE_LOOP_BACK)
        {
            sampleCanLoopback(can, base_flags);
        }
        else
        {
            std::cout << "[CAN] LOOP_BACK not advertised -- skipping loopback test.\n";
        }

        if (bt.feature & GS_CAN_FEATURE_GET_STATE)
        {
            printCanState(can, 0);
        }

        for (uint8_t ch = 0; ch < nch; ch++)
        {
            check(can.setMode(ch, GS_CAN_MODE_RESET, 0u), can.lastError(), "setMode RESET");
        }
        std::cout << "CAN channel(s) stopped.\n";
    }

    const unsigned can_errors = g_errors - can_errors_before;

    /* ------------------------------------------------------------------ */
    /*  Close all -- kernel drivers re-attached here on Linux              */
    /* ------------------------------------------------------------------ */
    if (lin_ok) { lin.close(); std::cout << "LIN closed.\n"; }
    if (can_ok) { can.close(); std::cout << "CAN closed.\n"; }
    if (aio_ok) { aio.close(); std::cout << "AIO closed.\n"; }

    /* ------------------------------------------------------------------ */
    /*  Result summary                                                      */
    /* ------------------------------------------------------------------ */
    std::cout << "\n====================================================================\n";
    std::cout << "Test summary\n";
    std::cout << "====================================================================\n";

    auto printResult = [](const char *name, bool ran, unsigned errors)
    {
        std::cout << "  " << std::left << std::setfill(' ') << std::setw(6) << name << "  ";
        if (!ran)        std::cout << "SKIPPED (interface not available)\n";
        else if (!errors) std::cout << "PASSED\n";
        else             std::cout << "FAILED (" << errors << " error(s))\n";
    };

    printResult("LIN", lin_ok, lin_errors);
    printResult("CAN", can_ok, can_errors);
    printResult("AIO", aio_ok, aio_errors);

    std::cout << "--------------------------------------------------------------------\n";
    if (g_errors == 0)
    {
        std::cout << "All tests PASSED.\n";
    }
    else
    {
        std::cout << "FAILED — " << g_errors << " error(s) total.\n";
    }
    std::cout << "====================================================================\n";

    return (g_errors == 0) ? 0 : 1;
}
