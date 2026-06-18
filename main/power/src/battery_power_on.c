/*
 * devices/main/power/src/battery_power_on.c  (reconstructed)
 *
 * The battery power-on / buck / charging supervisor cluster of power_service.cpp.
 *
 * These are PowerService C++ virtuals reached only through the service vtable
 * (primary vtable @0x184a88, slots 0x184af0..0x184b08), so Ghidra's auto-analysis
 * never carved them: the body addresses appear only as raw 8-byte pointers built
 * with ADRP+ADD-to-data, with no string-anchored xref. They were recovered by
 * walking the vtable by hand. OEM addresses:
 *
 *   FUN_00112560  battery_power_supervise    buck/fault watchdog
 *   FUN_00112fc0  battery_charging_poll      charger-mode evaluator
 *   FUN_00113130  battery_power_on_recovery  "Battery error!" recovery
 *   FUN_00113280  battery_power_on_sequence  power-on / charger-state supervisor
 *   FUN_00113580  battery_enter_charging     enter-charging
 *
 * Behaviour-oriented C (per-TU compilable); the PowerService object is reached
 * through the offset accessors in power_common.h. The buck pulses below all share
 * the same shape: drive the buck, wait 100 ms, then drop it again unless the
 * BQ25672 reports PGOOD.
 */
#define _POSIX_C_SOURCE 199309L

#include <time.h>

#include "power_common.h"
#include "power_service.h"

/* OEM constant: battery-reset grace window (DAT_0015d2d8), reused by
 * battery_charging_poll when it issues a reset. */
extern uint32_t DAT_0015d2d8;

/* The OEM blocks ~100 ms with clock_nanosleep(), retrying across EINTR. */
static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* Drive the buck and require PGOOD within 100 ms, else turn it back off and
 * log `msg` at power_service.cpp:`line`. */
static void buck_pulse_require_pgood(PowerService *self, void *sw, int line,
                                     const char *msg)
{
    if (!buck_is_enabled(sw))
        return;
    sleep_ms(100);
    if (!bq25672_read_pgood(PS_lipo_gauge(self))) {
        common_logf("devices/main/power/src/power_service.cpp", line, LOG_WARN, msg);
        buck_set_enable(sw, 0);
    }
}

/* ------------------------------------------------------------------ *
 *  battery_power_supervise  (OEM FUN_00112560)
 *
 *  State-machine-driven buck/fault watchdog. From a clean INVALID request it
 *  forces the buck on whenever the bike is actually powered (OPERATIONAL /
 *  UPDATING / ALARM); for a pending OPERATIONAL / UPDATING / ALARM request it
 *  arms the timestamped battery-fault flag that battery_power_on_recovery acts
 *  on. The OEM tests the standby sub-state and the fault byte together as one
 *  u16 at +0x120.
 * ------------------------------------------------------------------ */
void battery_power_supervise(PowerService *self)
{
    int req = StateManager_RequestedState(self);

    if (req == ST_INVALID) {
        if (PS_substate(self) == 1 && PS_fault(self) == 0)
            PS_fault(self) = 1;

        int cur = StateManager_CurrentState(self);
        if (cur != ST_OPERATIONAL && cur != ST_UPDATING && cur != ST_ALARM)
            return;

        buck_drive_on(PS_switch_ctrl(self));        /* OEM FUN_00142900 (GPIO) */
        if (!PS_buck_logged(self))
            common_logf("devices/main/power/src/switch_control.cpp", 0x52,
                        LOG_INFO, "BUCK %s", "ENABLED");
        PS_buck_logged(self) = 1;
        return;
    }

    if (req != ST_OPERATIONAL && req != ST_UPDATING && req != ST_ALARM)
        return;

    if (PS_fault(self) != 0)
        return;
    PS_fault(self)    = 2;
    PS_fault_ts(self) = clock_now_ns();
}

/* ------------------------------------------------------------------ *
 *  battery_charging_poll  (OEM FUN_00112fc0)
 *
 *  Evaluate the charger mode (Monitor::charger_decode_mode) while a charger is
 *  connected and drive the battery accordingly. Returns false when the charger
 *  is absent or reports mode 3 (so callers know charging is not active).
 *
 *    mode 0 : charger fault            -> latch fault flag = 3
 *    mode 1 : unidentified charger     -> CHARGER_CMD_IDENTIFY
 *    mode 2 : charging                 -> power battery off when fully charged,
 *                                         or one-shot reset when (re)inserted
 *    mode 3 : not charging / removed   -> battery off + back to STANDBY
 * ------------------------------------------------------------------ */
bool battery_charging_poll(PowerService *self)
{
    void *pc = PS_power_control(self);
    if (!power_control_charger_connected(pc))
        return false;

    void   *mon  = PS_monitor(self);
    uint8_t mode = charger_decode_mode(mon);

    if (mode == 2) {
        if (!monitor_battery_ins_det(mon) && !power_control_battery_is_off(pc)) {
            common_logf("devices/main/power/src/power_service.cpp", 0x29d,
                        LOG_INFO, "Battery is fully charged, powering off");
            battery_cmd0_off(pc);
        } else if (monitor_battery_ins_det(mon) && !PS_reset_done(self)) {
            battery_cmd6_reset(pc);
            power_control_set_reset_grace(pc, DAT_0015d2d8);
            monitor_clear_charger_mode(mon);
            PS_reset_done(self) = 1;
        }
    } else if (mode < 3) {
        if (mode == 0)
            PS_fault(self) = 3;
        else
            charger_cmd5_identify(pc);
    } else if (mode == 3 &&
               !timer_running(PS_tick_timer(self)) &&
               !power_control_battery_is_off(pc)) {
        battery_cmd0_off(pc);
        PS_substate(self) = 0;
        ps_timer_arm(PS_tick_timer(self), 3000);
        StateManager_ChangeState(self, ST_STANDBY);
    }

    return mode != 3;
}

/* ------------------------------------------------------------------ *
 *  battery_power_on_recovery  (OEM FUN_00113130, power_service.cpp:0x335)
 *
 *  The "Battery error!" handler. Latches the fault flag and, if a charger is
 *  connected and still within its retry window, pulses the buck (PGOOD-guarded)
 *  before handing back to the charging poll.
 * ------------------------------------------------------------------ */
void battery_power_on_recovery(PowerService *self)
{
    void *pc = PS_power_control(self);

    common_logf("devices/main/power/src/power_service.cpp", 0x335, LOG_ERR,
                "Battery error!");

    if (PS_fault(self) == 0)
        PS_fault(self) = 1;

    if (!power_control_charger_connected(pc))
        return;

    if (power_control_retry_pending(pc)) {
        void *sw = PS_switch_ctrl(self);
        buck_set_enable(sw, 1);
        buck_pulse_require_pgood(self, sw, 0x347,
            "No PGOOD received while charger is connected, turning the buck OFF");
    }

    battery_charging_poll(self);
}

/* ------------------------------------------------------------------ *
 *  battery_power_on_sequence  (OEM FUN_00113280, power_service.cpp:0x2ba-0x2ee)
 *
 *  Power-on / charger-state-transition supervisor, run from the tick path. Walks
 *  the bike from a pending power-on through the OPERATIONAL <-> CHARGING <->
 *  STANDBY transitions, driving the buck and emitting the operator-facing log
 *  lines at each edge.
 * ------------------------------------------------------------------ */
void battery_power_on_sequence(PowerService *self)
{
    void *pc = PS_power_control(self);

    /* Latched fault and the charger has since gone: just announce recovery. */
    if (PS_fault(self) == 1 && !power_control_charger_connected(pc)) {
        common_logf("devices/main/power/src/power_service.cpp", 0x2ba, LOG_INFO,
                    "In battery power on sequence recovery");
        return;
    }

    /* No power-on pending (idle request) or in MAINTENANCE: drive the buck to
     * whatever PowerControl asks and verify PGOOD. */
    if (StateManager_RequestedState(self) == ST_INVALID ||
        StateManager_CurrentState(self) == ST_MAINTENANCE) {
        void *sw = PS_switch_ctrl(self);
        buck_set_enable(sw, power_control_buck_needed(pc));
        buck_pulse_require_pgood(self, sw, 0x2c6,
            "No PGOOD received while charger is connected & battery is off, "
            "turning the buck OFF");
    }

    if (StateManager_CurrentState(self) == ST_OPERATIONAL &&
        !power_control_charger_connected(pc)) {
        common_logf("devices/main/power/src/power_service.cpp", 0x2cf, LOG_WARN,
                    "Battery powered off in operational state, tried turning  "
                    "the system off/on to quickly.");
    }

    if (!power_control_charger_connected(pc)) {
        monitor_clear_charger_mode(PS_monitor(self));
        if (monitor_battery_ins_det(PS_monitor(self)))
            PS_reset_done(self) = 0;
    } else if (battery_charging_poll(self)) {
        int cur = StateManager_CurrentState(self);
        if (cur != ST_CHARGING && cur != ST_UPDATING) {
            common_logf("devices/main/power/src/power_service.cpp", 0x2da, LOG_WARN,
                        "Charger connected, but not charging. Go to charging state");
            timer_stop(PS_tick_timer(self));
            StateManager_ChangeState(self, ST_CHARGING);
        }
        if (StateManager_CurrentState(self) == ST_CHARGING)
            eshifter_request_calibration(PS_eshifter_calib(self));
    }

    if (StateManager_CurrentState(self) == ST_CHARGING &&
        !power_control_charger_connected(pc)) {
        common_logf("devices/main/power/src/power_service.cpp", 0x2ee, LOG_WARN,
                    "Charging finished. Go to standby state");
        StateManager_ChangeState(self, ST_STANDBY);
        PS_substate(self) = 0;
        ps_timer_arm(PS_tick_timer(self), 3000);
    }

    if (StateManager_CurrentState(self) == ST_UPDATING &&
        !power_control_charger_connected(pc)) {
        battery_cmd1_on(pc);
    }
}

/* ------------------------------------------------------------------ *
 *  battery_enter_charging  (OEM FUN_00113580, power_service.cpp:~0x322)
 *
 *  Bring the system into the charging configuration: pulse the buck
 *  (PGOOD-guarded), park switch_control in state 4, request CAN low-power, then
 *  evaluate the charger and move to CHARGING if it is live.
 * ------------------------------------------------------------------ */
void battery_enter_charging(PowerService *self)
{
    void *sw = PS_switch_ctrl(self);

    if (power_control_buck_needed(PS_power_control(self))) {
        buck_set_enable(sw, 1);
        buck_pulse_require_pgood(self, sw, 0x322,
            "No PGOOD received while charging");
    }

    switch_control_set_state(sw, 4);
    low_power_request_can_low_power(PS_low_power(self));

    if (battery_charging_poll(self) &&
        StateManager_CurrentState(self) != ST_CHARGING &&
        StateManager_CurrentState(self) != ST_UPDATING) {
        timer_stop(PS_tick_timer(self));
        StateManager_ChangeState(self, ST_CHARGING);
    }
}
