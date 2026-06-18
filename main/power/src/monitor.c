/*
 * monitor.c — battery/charger telemetry monitor (devices/main/power, monitor.cpp)
 *
 * OEM: /usr/bin/power
 *   Monitor_ctor                       0x129520
 *   monitor_charger_connected_publish  0x128cc4
 *   monitor_charger_disconnected       0x12cde0
 *   monitor_charger_finished           0x12d790  (device/charger/finished)
 *   monitor_charger_progress_started   0x12d950  (device/charger/mode start)
 *   monitor_charger_progress_finished  0x12d3d0  (device/charger/mode finished)
 *   charger_decode_mode                0x1281d0  (== 0x128544)
 *   charger_decode_mode_update         0x128010  (clear-override helper)
 *   battery_status_decode              0x1268d0
 *   battery_warning_decode             0x1275a0
 *
 * Faithful translation of the decompiled logic. The std::vector<OdRegistration>,
 * the std::function decode callbacks and the std::string topics are modelled
 * through the shared OD interface. Field offsets, the bit extraction in the two
 * flag decoders and the Success/FailRetry/InProgress->0/1/2/3 mapping are taken
 * verbatim from the binary. Little-endian.
 *
 * The simple per-signal decoders (voltage/charging/health/capacity/temperature)
 * live in battery_decode.c — referenced here, not duplicated.
 */
#include "monitor.h"
#include "battery_decode.h"
#include "power_common.h"

#define MON_FILE "devices/main/power/src/monitor.cpp"

/* battery_decode.c bodies, registered as OD-signal callbacks below. They take a
 * struct battery_monitor*; the Monitor caches soc/charge_current/voltage at the
 * same OEM offsets, so the cast is layout-compatible. */
static void dec_charging  (struct Monitor *m, const uint8_t *f)
{ battery_charging_decode((struct battery_monitor *)m, f); }
static void dec_health    (struct Monitor *m, const uint8_t *f)
{ battery_health_decode((struct battery_monitor *)m, f); }
static void dec_capacity  (struct Monitor *m, const uint8_t *f)
{ battery_capacity_decode((struct battery_monitor *)m, f); }
static void dec_temp      (struct Monitor *m, const uint8_t *f)
{ battery_temperature_decode((struct battery_monitor *)m, f); }
static void dec_voltage   (struct Monitor *m, const uint8_t *f)
{ battery_voltage_decode((struct battery_monitor *)m, f); }
/* battery_cell_decode (0x12e2a0) is a no-op stub in the OEM image. */
static void dec_cell      (struct Monitor *m, const uint8_t *f)
{ (void)m; (void)f; }

/* ----------------------------------------------------------------------- *
 * Monitor_ctor — OEM 0x129520
 *
 * Builds the std::vector<OdRegistration> of primary-battery signals, each with
 * a decode->publish callback, then subscribes to the five device/charger
 * topics. (The OEM open-codes inline SSO std::string construction + the vector
 * grow path + a runtime_error on duplicate registration; modelled here as a
 * registration table.)
 * ----------------------------------------------------------------------- */
struct od_reg_entry { const char *signal; od_decode_fn decode; };

static const struct od_reg_entry MONITOR_SIGNALS[] = {
    { "battery_primary_battery_status",      battery_status_decode }, /* 0x12e670 -> 0x1268d0 */
    { "battery_primary_battery_charging",    dec_charging          }, /* 0x12e310 -> 0x128730 */
    { "battery_primary_battery_health",      dec_health            }, /* 0x12e370 -> 0x1289c0 */
    { "battery_primary_battery_capacity",    dec_capacity          }, /* 0x12e3d0 -> 0x12d150 */
    { "battery_primary_battery_cell",        dec_cell              }, /* 0x12e2a0 -> 0x12e2a0 */
    { "battery_primary_battery_temperature", dec_temp              }, /* 0x12e430 -> 0x12da90 */
    { "battery_primary_battery_voltage",     dec_voltage           }, /* 0x12e490 -> 0x1285c0 */
    { "battery_primary_battery_warning",     battery_warning_decode}, /* 0x12e4f0 -> 0x1275a0 */
    { "charger_state",                       0                     }, /* 0x12e550 (state init) */
    { "charger_er_state",                    0                     }, /* 0x12e5b0 (er_state)   */
};

void Monitor_ctor(struct Monitor *m, void *od, void *ipc,
                  void *timer_factory, void *power_control)
{
    unsigned i;

    m->od            = od;
    m->ipc           = ipc;
    m->charger_client= timer_factory;   /* OEM param order; +0x30 holds it      */
    m->power_control = power_control;
    m->charger_in    = power_control;

    /* the OD-registration vector */
    for (i = 0; i < sizeof MONITOR_SIGNALS / sizeof MONITOR_SIGNALS[0]; i++)
        od_register_signal(m, MONITOR_SIGNALS[i].signal, MONITOR_SIGNALS[i].decode);

    /* reset the cached charger/battery state (OEM clears these inline) */
    m->mode_count_97 = 0;
    m->retry_count_98 = 0;
    m->charge_current = 0;
    m->charging = 0;
    m->fault_uvp_92 = 0;
    m->state_84 = 3;
    m->fault_flag_86 = 0xff;
    m->st_0x8e = 0;
    m->connected_93 = 0;
    m->identify_sent_94 = 0;
    m->state_nibble_95 = 0;
    m->fw_update_96 = 0;

    /* device/charger subscriptions (timer-driven mode poll + the publishes) */
    od_subscribe(m, "device/charger/connected",
                 (od_topic_fn)monitor_charger_connected_publish);
    od_subscribe(m, "device/charger/finished",
                 (od_topic_fn)monitor_charger_finished);
    od_subscribe(m, "update/background_update/progress_info/charger",
                 (od_topic_fn)monitor_charger_progress_finished);
    od_subscribe(m, "update/stage2/device_update_started",
                 (od_topic_fn)monitor_charger_progress_started);
    /* device/charger/{voltage,current,mode} timer poll wired via timer_factory. */
}

/* ----------------------------------------------------------------------- *
 * monitor_charger_connected_publish — OEM 0x128cc4
 *
 * On a device/charger/connected frame: latch +0x93 on the first edge and publish
 * device/charger/connected(bool). Then, while the charger-in gate is clear,
 * publish device/charger/current (u16 frame[1]) and device/charger/voltage
 * (u16 frame[0]) whenever they change, caching last-published at +0x88 / +0x8a.
 * ----------------------------------------------------------------------- */
void monitor_charger_connected_publish(struct Monitor *m, const uint16_t *frame)
{
    bool gate;

    /* (charger_in vtable+0x18)(...,0): refresh the charger-in gate. */
    gate = false;   /* (*(m->charger_in)+0x18)(m->charger_in, 0) — modelled */
    (void)gate;

    if (m->connected_93 == 0) {
        common_logf(MON_FILE, 0x19f, LOG_INFO, "Charger connected");
        m->connected_93 = 1;
        od_pub_named_bool(m->ipc, "device/charger/connected", m->connected_93 != 0);
    }

    /* (power_control vtable+0x28)(): true => suppress voltage/current publish. */
    if (/*(*(m->power_control)+0x28)()*/ 0 == 0) {
        if (frame[1] != m->last_current_88) {
            od_pub_u16(m->ipc, "device/charger/current", frame[1], 1, 1);
            m->last_current_88 = frame[1];
        }
        if (frame[0] != m->last_voltage_8a) {
            od_pub_u16(m->ipc, "device/charger/voltage", frame[0], 1, 1);
            m->last_voltage_8a = frame[0];
        }
        /* (power_control vtable+0x18)(): release the gate. */
    }
}

/* ----------------------------------------------------------------------- *
 * monitor_charger_disconnected — OEM 0x12cde0
 *
 * On charger removal: clear +0x93, publish device/charger/connected(false),
 * reset the charger sub-state (FUN_00127c20), and re-init +0x84/+0x86.
 * ----------------------------------------------------------------------- */
void monitor_charger_disconnected(struct Monitor *m)
{
    common_logf(MON_FILE, 0x70, LOG_INFO, "Charger disconnected");
    m->connected_93 = 0;
    od_pub_named_bool(m->ipc, "device/charger/connected", m->connected_93 != 0);
    /* FUN_00127c20(m): reset cached charger telemetry / counters. */
    m->state_84 = 3;
    m->fault_flag_86 = 0xff;
}

/* ----------------------------------------------------------------------- *
 * monitor_charger_finished — OEM 0x12d790  (device/charger/finished)
 *
 * Parses the JSON string and maps the charger's reported result:
 *   "Idle" | "FailRetry" | "Success" | "Fail"  -> background update finished
 *                                                 (+0x96 = 0)
 *   "InProgress"                               -> in progress (+0x96 = 1)
 * Anything else is ignored. The mode strings live at .rodata 0x15ef80..0x15efa0.
 * ----------------------------------------------------------------------- */
void monitor_charger_finished(struct Monitor *m, const void *json)
{
    const char *mode;

    /* OEM: if payload type != string (json[0]!=3) -> ignore. */
    mode = (const char *)json;   /* extracted std::string (FUN_001199c0) */
    if (mode == 0)
        return;

    if (od_streq(mode, "Idle")      || od_streq(mode, "FailRetry") ||
        od_streq(mode, "Success")   || od_streq(mode, "Fail")) {
        common_logf(MON_FILE, 0x84, LOG_INFO,
                    "Background update for charger is finished");
        m->fw_update_96 = 0;
    } else if (od_streq(mode, "InProgress")) {
        common_logf(MON_FILE, 0x88, LOG_INFO,
                    "Background update for charger is in progress");
        m->fw_update_96 = 1;
    }
}

/* ----------------------------------------------------------------------- *
 * monitor_charger_progress_started — OEM 0x12d950 (update/stage2 start)
 *
 * If the started-device string mentions the charger, set +0x96 = 1.
 * ----------------------------------------------------------------------- */
void monitor_charger_progress_started(struct Monitor *m, const void *json)
{
    const char *dev = (const char *)json;   /* extracted std::string */

    /* OEM matches two substrings (the charger device-id keys) via std::find. */
    if (dev && od_contains_charger(dev)) {
        common_logf(MON_FILE, 0x9b, LOG_INFO, "%s update started", dev);
        m->fw_update_96 = 1;
    }
}

/* ----------------------------------------------------------------------- *
 * monitor_charger_progress_finished — OEM 0x12d3d0 (update progress info)
 *
 * Walks the JSON object for the "device" key; if its string value mentions the
 * charger, mark the FW update finished (+0x96 = 0).
 * ----------------------------------------------------------------------- */
void monitor_charger_progress_finished(struct Monitor *m, const void *json)
{
    const char *dev = (const char *)json;   /* "device" value (object lookup) */

    if (dev && od_contains_charger(dev)) {
        common_logf(MON_FILE, 0xa9, LOG_INFO, "%s update finished", dev);
        m->fw_update_96 = 0;
    }
}

/* ----------------------------------------------------------------------- *
 * charger_decode_mode_update — OEM 0x128010
 *
 * Reset the mode counter (+0x97/+0x98 word) and, if the OD override `key` is
 * present, erase it. Called from charger_decode_mode when a FW update is active.
 * ----------------------------------------------------------------------- */
static void charger_decode_mode_update(struct Monitor *m)
{
    *(uint16_t *)&m->mode_count_97 = 0;   /* clears +0x97 and +0x98 */
    if (od_override_query("charge_retry"))   /* DAT_00189108 std::string */
        od_override_erase("charge_retry");
}

/* ----------------------------------------------------------------------- *
 * charger_decode_mode — OEM 0x1281d0 (== 0x128544)
 *
 * Periodic (1000 ms) charger mode state machine. Returns the device/charger/mode
 * code published downstream:
 *   0 = idle / sampling, 1 = charging (Success), 2 = FailRetry, 3 = InProgress.
 * mode 2 ("FailRetry") sets the generic-fault path; mode 3 retries up to 3 times.
 * ----------------------------------------------------------------------- */
uint8_t charger_decode_mode(struct Monitor *m)
{
    uint8_t mode;

    /* FW update active: just reset the counter and report idle. */
    if (m->fw_update_96 != 0) {
        charger_decode_mode_update(m);
        return 0;
    }

    /* High SoC (>=0x61) with no charge current -> request identify once. */
    if (m->soc >= 0x61 && m->charge_current == 0) {
        *(uint16_t *)&m->mode_count_97 = 0;
        if (m->identify_sent_94 == 0) {
            m->identify_sent_94 = 1;
            /* publish device/charger/finished(bool) = identify_sent. */
            od_pub_named_bool(m->ipc, "device/charger/finished",
                              m->identify_sent_94 != 0);
        }
        return 2;
    }

    mode = 0;

    /* Charge current too high (>=0xc9 = 200): reset the sample counter. */
    if (m->charge_current >= 0xc9) {
        *(uint16_t *)&m->mode_count_97 = 0;
        return 0;
    }

    /* Sample the mode for 20 ticks (0x14); advance the retry counter. */
    {
        uint8_t retry = m->retry_count_98;
        m->mode_count_97 = (uint8_t)(m->mode_count_97 + 1);

        if (m->mode_count_97 == 0x14 && retry < 3) {
            m->mode_count_97 = 0;
            m->retry_count_98 = (uint8_t)(retry + 1);

            if (m->charging != 0) {
                if (m->soc != 0 || m->fault_uvp_92 == 0) {
                    common_logf(MON_FILE, 0x244, LOG_WARN,
                        "Battery has generic fault bit set, need to reset battery");
                    m->retry_count_98 = 3;
                    return 2;
                }
                common_logf(MON_FILE, 0x242, LOG_INFO,
                    "Battery is empty and in fault (UVP), let's continue with charging");
            }

            /* Trickle-charge case: empty pack, no fault, state nibble == 2. */
            if (m->soc == 0 && m->fault_flag_86 == 0 && m->state_nibble_95 == 2) {
                m->retry_count_98 = 0;
                common_logf(MON_FILE, 0x24c, LOG_INFO, "Trickle charging");
            } else {
                mode = 1;
            }
        } else if (retry == 3) {
            m->mode_count_97 = 0;
            mode = 3;
            if (!od_override_query("charge_retry")) {
                od_override_mark("charge_retry");
                common_logf(MON_FILE, 0x25a, LOG_WARN,
                            "Retry to charge failed after 3 retries");
            }
        }
    }

    return mode;
}

/* ----------------------------------------------------------------------- *
 * battery_status_decode — OEM 0x1268d0
 *
 * Decodes the primary-battery STATUS frame: ~33 boolean flags from frame[0..6]
 * plus the cached named fields. The boolean topics are runtime std::strings
 * (static-init globals, not statically recoverable) — published via
 * od_pub_named_bool with documented placeholder names. The named/pinned fields
 * are written to the fixed Monitor offsets and match the OEM exactly:
 *   charging   = frame[0]>>7        (+0x91)
 *   INS-DET    = frame[6]>>5 & 1    (+0x8f)
 *   INS-DET-FAIL = frame[6]>>4 & 1  (+0x90)
 *   state nibble = frame[6] & 0x0f  (+0x95)
 * ----------------------------------------------------------------------- */
void battery_status_decode(struct Monitor *m, const uint8_t *f)
{
    void *ipc = m->ipc;

    /* charging flag (+0x91): frame[0]>>7. When clear, drop the "charging"
     * override key so downstream stops treating us as charging. */
    m->charging = (uint8_t)(f[0] >> 7);
    if (m->charging == 0) {
        if (od_override_query("charging"))
            od_override_erase("charging");
    }

    /* derived UVP/empty-fault flag (+0x92): frame[1] bit1 ? 1 : bit2. */
    m->fault_uvp_92 = (f[1] >> 1 & 1) ? 1u : (uint8_t)(f[1] >> 2 & 1);

    /* the ~33 published booleans, in OEM publish order. Bit extraction is
     * verbatim; topic names below are documented placeholders (status.bN_bM). */
    od_pub_named_bool(ipc, "battery_status.charging",   f[0] >> 7);       /* charging */
    od_pub_named_bool(ipc, "battery_status.b1_b0",      f[1]      & 1);
    od_pub_named_bool(ipc, "battery_status.b1_b2",      f[1] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_status.b1_b1",      f[1] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_status.b1_b3",      f[1] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_status.b1_b4",      f[1] >> 4 & 1);
    od_pub_named_bool(ipc, "battery_status.b1_b5",      f[1] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b0",      f[2]      & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b1",      f[2] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b2",      f[2] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b3",      f[2] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b5",      f[2] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_status.b2_b6",      f[2] >> 6 & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b0",      f[3]      & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b1",      f[3] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b2",      f[3] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b3",      f[3] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b5",      f[3] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_status.b3_b6",      f[3] >> 6 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b0",      f[4]      & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b1",      f[4] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b2",      f[4] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b3",      f[4] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b4",      f[4] >> 4 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b5",      f[4] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b6",      f[4] >> 6 & 1);
    od_pub_named_bool(ipc, "battery_status.b4_b7",      f[4] >> 7);
    od_pub_named_bool(ipc, "battery_status.b5_b0",      f[5]      & 1);
    od_pub_named_bool(ipc, "battery_status.b5_b1",      f[5] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_status.b5_b4",      f[5] >> 4 & 1);
    od_pub_named_bool(ipc, "battery_status.b5_b5",      f[5] >> 5 & 1);

    /* cached named fields (no publish for 0x8e; INS-DET / INS-DET-FAIL / nibble
     * are cached and the latter two also re-published below). */
    m->st_0x8e        = (uint8_t)(f[0] >> 5 & 1);
    m->ins_det        = (uint8_t)(f[6] >> 5 & 1);  /* INS-DET      */
    m->ins_det_fail   = (uint8_t)(f[6] >> 4 & 1);  /* INS-DET-FAIL */
    m->state_nibble_95= (uint8_t)(f[6]      & 0xf);/* state nibble */

    od_pub_named_bool(ipc, "battery_status.ins_det_fail", f[6] >> 4 & 1); /* INS-DET-FAIL */
    od_pub_named_bool(ipc, "battery_status.b6_b6",        f[6] >> 6 & 1);
    od_pub_named_bool(ipc, "battery_status.b6_b7",        f[6] >> 7);
}

/* ----------------------------------------------------------------------- *
 * battery_warning_decode — OEM 0x1275a0
 *
 * Decodes the primary-battery WARNING/ALARM frame: 18 boolean alarm flags from
 * frame[0..4], all published via od_pub_named_bool. No cached fields. Bit
 * extraction is verbatim; topic names are documented placeholders.
 * ----------------------------------------------------------------------- */
void battery_warning_decode(struct Monitor *m, const uint8_t *f)
{
    void *ipc = m->ipc;

    od_pub_named_bool(ipc, "battery_warning.b0_b7", f[0] >> 7);
    od_pub_named_bool(ipc, "battery_warning.b1_b1", f[1] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_warning.b1_b2", f[1] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_warning.b1_b3", f[1] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_warning.b1_b4", f[1] >> 4 & 1);
    od_pub_named_bool(ipc, "battery_warning.b2_b0", f[2]      & 1);
    od_pub_named_bool(ipc, "battery_warning.b2_b1", f[2] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_warning.b2_b2", f[2] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_warning.b2_b3", f[2] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_warning.b2_b5", f[2] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b0", f[3]      & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b1", f[3] >> 1 & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b2", f[3] >> 2 & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b3", f[3] >> 3 & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b5", f[3] >> 5 & 1);
    od_pub_named_bool(ipc, "battery_warning.b3_b6", f[3] >> 6 & 1);
    od_pub_named_bool(ipc, "battery_warning.b4_b0", f[4]      & 1);
    od_pub_named_bool(ipc, "battery_warning.b4_b1", f[4] >> 1 & 1);
}
