/*
 * lipo_control.c — LiPo (backup-cell) fuel gauge + BQ25672 charger control
 *
 * OEM: /usr/bin/power
 *   bq27542 attribute reads        0x1213c0 .. 0x121bc0 (int), 0x122450/590/6d0 (str)
 *   lipo info publisher            0x12ba60
 *   bq25672_read_pgood             0x121e20
 *   bq25672_read_muxswitch_tps2121 0x121f80
 *   bq25672_read_vac1_chrgstat0    0x123f90
 *   bq25672_read_register          0x122cb0 (+ 0x123290 wrapper for CHRG_STAT)
 *   lipo_bq25672_write_regs_shipping 0x1220e0
 *   lipo_bq25672_write_regs_standby  0x122290
 *   buck_set_enable                0x11d920 (switch_control.cpp:0x52)
 *   lipo_buck_enable_pgood         0x1136e0 (power_service.cpp)
 *
 * Faithful translation of the decompiled logic. sysfs paths, the BQ25672 register
 * profile tables, the CHRG_STAT bit math and the PGOOD-retry / fault-recovery
 * control flow are taken verbatim from the binary. The std::string/std::fstream
 * plumbing and the OD/MQTT framework are modelled through the documented
 * power_common.h helpers (behaviour-equivalent, not byte-identical).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lipo_control.h"

#define LIPO_CPP "devices/main/power/src/lipo_control.cpp"
#define PSVC_CPP "devices/main/power/src/power_service.cpp"
#define SWC_CPP  "devices/main/power/src/switch_control.cpp"
#define SYSFS_H  "devices/main/common/inc/sysfs_utils.h"

/* ---- cross-module helpers (other power TUs / vendor) -------------------- */

/* StateManager + state names (state_manager.cpp). */
extern void StateManager_ChangeState(void *sm, int state);
extern void StateManager_StateName(void *out, int state);

/* battery command set (battery_cmd.cpp). All take PS_power_control (+0x50). */
extern void battery_cmd1_on(void *power_control);          /* 0x1335d0 */
extern void battery_cmd6_reset(void *power_control);       /* 0x133ac0 */
extern void battery_cmd9_clear_fault(void *power_control); /* 0x1337d0 */
extern bool get_is_primary_inserted(void *power_control);  /* 0x133450 */
extern bool battery_is_on(void *power_control);            /* 0x133470 */
extern uint16_t battery_sys_voltage(void *power_control);  /* 0x1332e0 */
extern bool battery_is_voltage_stable(void *power_control);/* 0x1333f0 */

/* Monitor (telemetry) reads (monitor.cpp, object = PS_monitor +0x150). */
extern uint16_t monitor_pack_voltage(void *monitor);       /* 0x128160 */
extern bool monitor_is_battery_dsg(void *monitor);         /* 0x128140 */
extern bool monitor_ins_det(void *monitor);                /* 0x128170 */
extern bool monitor_ins_det_fail(void *monitor);           /* 0x128190 */

/* requested-state resolver for the current substate (power_service.cpp 0x11ac80). */
extern int  ps_target_state(PowerService *self);           /* 0x11ac80 */
extern void ps_set_switches(void *switch_ctrl, int state); /* 0x11e430 */
extern void ps_publish_done(PowerService *self);           /* ipc vtable+0x10 hook */

/* switch_control GPIO direction toggle (gpio.cpp dir = 0x147d60). The BUCK enable
 * is an open-drain line: "enable" releases (dir=!active_low), "disable" drives. */
extern void gpio_set_dir(void *gpio, bool level);          /* 0x147d60 */

/* monotonic clock + sleep helpers (std::this_thread / chrono). */
extern uint64_t ps_now_ns(void);                           /* 0x109080 */
extern void     ps_sleep_ns(long sec, long nsec);          /* nanosleep loop */

/* od_table warning-bit publish (FUN_0013e220): publishes a bool to a static topic. */
extern void od_warn_publish(int topic_id, bool v);

/* OD/MQTT string publish (ipc vtable+0x20, value tag 3). Not in power_common.h
 * (which only declares the numeric od_pub_*); declared here for this TU. */
extern void od_pub_str(void *ipc, const char *topic, const char *value);

/* switch_control object layout (switch_control.cpp). */
struct switch_control {
    uint8_t  _pad00[0x40];
    void    *enable_gpio;   /* +0x40 BUCK enable line (passed to gpio dir toggle) */
    uint8_t  _pad48[0xe0 - 0x48];
    uint8_t  buck_enabled;  /* +0xe0 cached BUCK enable state */
};

/* ====================================================================== *
 *  bq27542 fuel-gauge single-attribute reads
 * ====================================================================== */

/*
 * Shared body for the integer attributes: open BQ27542_SYSFS/<attr> "r",
 * fscanf("%d"). On open failure return -1 unless errno==0 (then the zero-init
 * value, i.e. 0); on a short scan return -1.  (OEM 0x1213c0 family, verbatim.)
 */
static int lipo_gauge_read_int(const char *attr)
{
    char path[0x80];
    int  value = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", BQ27542_SYSFS, attr);
    f = fopen(path, "r");
    if (f == NULL)
        return (errno != 0) ? -1 : value;

    if (fscanf(f, "%d", &value) == 1) {
        fclose(f);
        return value;
    }
    fclose(f);
    return -1;
}

int lipo_gauge_read_charge_now(void)         { return lipo_gauge_read_int("charge_now");         } /* 0x1214c0 */
int lipo_gauge_read_charge_full_design(void) { return lipo_gauge_read_int("charge_full_design"); } /* 0x1215c0 */
int lipo_gauge_read_charge_full(void)        { return lipo_gauge_read_int("charge_full");        } /* 0x1216c0 */
int lipo_gauge_read_capacity(void)           { return lipo_gauge_read_int("capacity");           } /* 0x1213c0 */
int lipo_gauge_read_power_avg(void)          { return lipo_gauge_read_int("power_avg");          } /* 0x1217c0 */
int lipo_gauge_read_voltage_now(void)        { return lipo_gauge_read_int("voltage_now");        } /* 0x1218c0 */
int lipo_gauge_read_current_now(void)        { return lipo_gauge_read_int("current_now");        } /* 0x1219c0 */
int lipo_gauge_read_cycle_count(void)        { return lipo_gauge_read_int("cycle_count");        } /* 0x121ac0 */
/* NB: temperature is the hwmon node, not a power_supply attribute. */
int lipo_gauge_read_temp(void)               { return lipo_gauge_read_int("hwmon1/temp1_input"); } /* 0x121bc0 */

/*
 * Shared body for the string attributes: fscanf("%s"). On any failure the OEM
 * returns the empty std::string. (OEM 0x122590 / 0x1226d0 / 0x122450, verbatim.)
 */
static void lipo_gauge_read_str(const char *attr, char *out, size_t cap)
{
    char path[0x80];
    FILE *f;

    if (cap == 0)
        return;
    out[0] = '\0';

    snprintf(path, sizeof(path), "%s/%s", BQ27542_SYSFS, attr);
    f = fopen(path, "r");
    if (f == NULL)
        return;                          /* errno==0 path also yields "" here */

    if (fscanf(f, "%63s", out) != 1)
        out[0] = '\0';
    fclose(f);
}

void lipo_gauge_read_health(char *out, size_t cap)         { lipo_gauge_read_str("health",         out, cap); } /* 0x122590 */
void lipo_gauge_read_status(char *out, size_t cap)         { lipo_gauge_read_str("status",         out, cap); } /* 0x1226d0 */
void lipo_gauge_read_capacity_level(char *out, size_t cap) { lipo_gauge_read_str("capacity_level", out, cap); } /* 0x122450 */

/* ====================================================================== *
 *  lipo info publisher (OEM 0x12ba60)
 * ====================================================================== */

/*
 * Read every gauge attribute and publish the power/battery/lipo/info topics
 * (qos 0, retain 0). Integers via od_pub_int (vtable tag 5), strings via the
 * std::string publish (tag 3). After the gauge dump it reads BQ25672 CHRG_STAT_0
 * and CHRG_STAT_1 (register 0x20) and fans the bits out to the warning OD table.
 */
void lipo_publish_info(PowerService *self)
{
    void *ipc = PS_ipc(self);
    char  s[64];
    int   rc;

    od_pub_int(ipc, "power/battery/lipo/info/soc",                lipo_gauge_read_charge_now(),         0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/charge_full_design", lipo_gauge_read_charge_full_design(), 0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/charge_full",        lipo_gauge_read_charge_full(),        0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/capacity",           lipo_gauge_read_capacity(),           0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/current_now",        lipo_gauge_read_current_now(),        0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/voltage",            lipo_gauge_read_voltage_now(),        0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/cycles",             lipo_gauge_read_cycle_count(),        0, 0);

    lipo_gauge_read_health(s, sizeof(s));
    /* string publish (vtable tag 3) — modelled here as a u16-less string pub. */
    od_pub_str(ipc, "power/battery/lipo/info/health", s);

    lipo_gauge_read_status(s, sizeof(s));
    od_pub_str(ipc, "power/battery/lipo/info/status", s);

    od_pub_int(ipc, "power/battery/lipo/info/temp",    lipo_gauge_read_temp(),     0, 0);
    od_pub_int(ipc, "power/battery/lipo/info/pwr_avg", lipo_gauge_read_power_avg(), 0, 0);

    lipo_gauge_read_capacity_level(s, sizeof(s));
    od_pub_str(ipc, "power/battery/lipo/info/capacity_lvl", s);

    /*
     * CHRG_STAT_0 (BQ25672 register 0x20) warning fan-out. The publisher reads
     * the register byte and publishes each bit (MSB..LSB) to a fixed OD-table
     * warning topic; on a read failure it publishes a single fault flag (id 7).
     * The warning topics live in the OD table (devices/main/power/od_table), so
     * only the bit indices are reconstructed here.
     */
    rc = bq25672_read_register(self, 0x20);
    if (rc >= 0) {
        uint8_t r = (uint8_t)rc;
        od_warn_publish(0, (r >> 7) & 1);
        od_warn_publish(1, (r >> 6) & 1);
        od_warn_publish(2, (r >> 5) & 1);
        od_warn_publish(3, (r >> 4) & 1);
        od_warn_publish(4, (r >> 3) & 1);
        od_warn_publish(5, (r >> 2) & 1);
        od_warn_publish(6, (r >> 1) & 1);
        od_warn_publish(7, (r >> 0) & 1);
        od_warn_publish(8, 0);           /* "read failed" flag -> false */
    } else {
        od_warn_publish(8, 1);
    }

    /* Second pass: CHRG_STAT_1 bits (same register read, different OD entries). */
    rc = bq25672_read_register(self, 0x20);
    if (rc >= 0) {
        uint8_t r = (uint8_t)rc;
        od_warn_publish(9,  (r >> 7) & 1);
        od_warn_publish(10, (r >> 6) & 1);
        od_warn_publish(11, (r >> 5) & 1);
        od_warn_publish(12, (r >> 4) & 1);
        od_warn_publish(13, (r >> 2) & 1);
        od_warn_publish(8,  0);
    } else {
        od_warn_publish(8, 1);
    }
}

/* ====================================================================== *
 *  BQ25672 charger sysfs status reads
 * ====================================================================== */

/*
 * Scan BQ25672_SYSFS/gpios for a line, returning true if the needle is present.
 * Used by PGOOD ("pwr-good = 1") and MuxSwitch ("tps2121st = 1"). The OEM first
 * stat()s the path and logs "Can't open"/"does not exist" via sysfs_utils.h.
 */
static bool bq25672_gpio_line_set(const char *needle)
{
    char  line[0x40];
    const char *path = BQ25672_SYSFS "/gpios";
    FILE *f;

    /* OEM stat() existence check (sysfs_utils.h:0x92). */
    f = fopen(path, "r");
    if (f == NULL) {
        common_logf(SYSFS_H, 0x8f, LOG_ERR, "Can't open %s", path);
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

/* OEM 0x121e20 — PGOOD asserted? */
bool bq25672_read_pgood(void *lipo_gauge)
{
    (void)lipo_gauge;
    return bq25672_gpio_line_set("pwr-good = 1");
}

/* OEM 0x121f80 — TPS2121 power-mux switched to the auxiliary input? */
bool bq25672_read_muxswitch_tps2121(void *lipo_gauge)
{
    (void)lipo_gauge;
    return bq25672_gpio_line_set("tps2121st = 1");
}

/*
 * OEM 0x122cb0 — read register `reg` from BQ25672_SYSFS/registers. The dump is a
 * text file of "[0xNN] ... = 0xVV" lines; find the line for `reg`, then the
 * " = 0x" token and strtol(base 16) the value. Returns the byte or -1.
 */
int bq25672_read_register(void *self, uint8_t reg)
{
    char  line[0x80];
    char  tag[16];
    const char *path = BQ25672_SYSFS "/registers";
    FILE *f;
    int   result = -1;

    (void)self;
    snprintf(tag, sizeof(tag), "[0x%02X]", reg);

    f = fopen(path, "r");
    if (f == NULL)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, tag);
        if (p) {
            char *eq = strstr(line, " = 0x");
            if (eq)
                result = (int)strtol(eq + 5, NULL, 10);
            else
                common_logf(LIPO_CPP, 0xfc, LOG_WARN,
                            "Unable to parse register value 0x%02X", reg);
            break;
        }
    }
    fclose(f);
    return result;
}

/*
 * OEM 0x123f90 — VAC1 (external 5 V) present? Reads register CHRG_STAT_0 from
 * BQ25672_SYSFS/registers, strtol(base 16), returns bit 1 (VAC1_PRESENT_STAT).
 */
bool bq25672_read_vac1_chrgstat0(void *low_power)
{
    char  line[0x80];
    const char *path = BQ25672_SYSFS "/registers";
    FILE *f;
    int   reg = -1;

    (void)low_power;
    f = fopen(path, "r");
    if (f == NULL)
        return false;

    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "CHRG_STAT_0");
        if (p) {
            char *eq = strchr(line, '=');
            if (eq)
                reg = (int)strtol(eq + 2, NULL, 16);   /* skip "= " then hex */
            break;
        }
    }
    fclose(f);
    return ((reg >> 1) & 1) != 0;
}

/* ====================================================================== *
 *  BQ25672 register profiles  (write "%02X %02X" pairs to .../registers)
 * ====================================================================== */

/*
 * Each profile starts by writing register 0x01 = 0x01 (charger-config reset),
 * then a small fixed reg/value list. The pairs are emitted as ASCII "RR VV"
 * lines into BQ25672_SYSFS/registers ("a" append).
 *   shipping (OEM 0x1220e0):  01=01, 02=0x7A, 0A=0x20
 *   standby  (OEM 0x122290):  01=01, 02=0xAE, 0A=0x23
 * (reg 0x02 = charge-current limit, reg 0x0A = recharge/term control.)
 */
static void lipo_bq25672_write_profile(const uint8_t pairs[][2], int n, int errline)
{
    const char *path = BQ25672_SYSFS "/registers";
    char  buf[0x10];
    int   i;

    for (i = 0; i < n; i++) {
        FILE *f;
        snprintf(buf, sizeof(buf), "%02X %02X", pairs[i][0], pairs[i][1]);
        f = fopen(path, "a");
        if (f == NULL) {
            if (errno != 0)
                common_logf(LIPO_CPP, errline, LOG_ERR,
                            "LiPo Register Write failed: %s", buf);
            continue;
        }
        if (fprintf(f, "%s", buf) < 0)
            common_logf(LIPO_CPP, errline, LOG_ERR,
                        "LiPo Register Write failed: %s", buf);
        fclose(f);
    }
}

/* OEM 0x1220e0. */
void lipo_bq25672_write_regs_shipping(void)
{
    static const uint8_t pairs[][2] = {
        { 0x01, 0x01 }, { 0x02, 0x7A }, { 0x0A, 0x20 },
    };
    lipo_bq25672_write_profile(pairs, 3, 0xb9);
}

/* OEM 0x122290. */
void lipo_bq25672_write_regs_standby(void)
{
    static const uint8_t pairs[][2] = {
        { 0x01, 0x01 }, { 0x02, 0xAE }, { 0x0A, 0x23 },
    };
    lipo_bq25672_write_profile(pairs, 3, 0xc3);
}

/* ====================================================================== *
 *  BUCK enable  (OEM 0x11d920, switch_control.cpp:0x52)
 * ====================================================================== */

/*
 * Toggle the BQ25672 BUCK enable GPIO. The line is open-drain: "enable" releases
 * it (dir = !active_low), "disable" drives it (dir = active_low). Logs the new
 * state only on an actual change (cached at switch_ctrl+0xe0).
 */
void buck_set_enable(void *switch_ctrl, bool enable)
{
    struct switch_control *sc = (struct switch_control *)switch_ctrl;

    if (!enable) {
        gpio_set_dir(sc->enable_gpio, false);   /* drive: dir = active_low(0) base */
        if (sc->buck_enabled)
            common_logf(SWC_CPP, 0x52, LOG_INFO, "BUCK %s", "DISABLED");
    } else {
        gpio_set_dir(sc->enable_gpio, true);    /* release: dir = !active_low      */
        if (!sc->buck_enabled)
            common_logf(SWC_CPP, 0x52, LOG_INFO, "BUCK %s", "ENABLED");
    }
    sc->buck_enabled = enable;
}

/* ====================================================================== *
 *  lipo_buck_enable_pgood  (OEM 0x1136e0, power_service.cpp)
 * ====================================================================== */

/* Dump the full status block the OEM logs at each phase (power_service.cpp). */
static void lipo_log_status(PowerService *self, int v_line, int dsg_line,
                            int vac_line, int ins_line)
{
    void *pc  = PS_power_control(self);
    void *mon = PS_monitor(self);

    common_logf(PSVC_CPP, v_line, LOG_INFO, "SysVoltage: %d PackVoltage: %d Stable: %d",
                battery_sys_voltage(pc), monitor_pack_voltage(mon),
                battery_is_voltage_stable(pc));
    common_logf(PSVC_CPP, dsg_line, LOG_INFO, "IsBatteryDSG: %d", monitor_is_battery_dsg(mon));
    common_logf(PSVC_CPP, vac_line, LOG_INFO, "IsVAC1Present: %d",
                bq25672_read_vac1_chrgstat0(PS_low_power(self)));
    common_logf(PSVC_CPP, ins_line, LOG_INFO,
                "IsPrimaryInserted: %d BatteryINS-DET: %d BatteryINS-DET FAIL: %d",
                get_is_primary_inserted(pc), monitor_ins_det(mon), monitor_ins_det_fail(mon));
}

/*
 * The standby "turn-on" sequence. `self` points at the PowerService* (the OEM
 * passes &this). For a small set of target states it tries to clear the battery
 * fault flags, turn the battery on, fall back to a full battery RESET if the
 * battery refuses to come on, then brings up the BUCK with a PGOOD retry loop and
 * — once VAC1 is present and the pack voltage is stable — flips the load switches
 * and commits the next state.
 */
void lipo_buck_enable_pgood(PowerService **selfp)
{
    PowerService *self = *selfp;
    void *pc;
    int   target;
    uint8_t substate;
    int   attempt;

    target = ps_target_state(self);

    /* Only the shipping/standby/charging/maintenance targets (bitmask 0x69 over
     * states 0..6) take the LiPo path; everything else just publishes done. */
    if (target > 6 || ((1u << (target & 0x3f)) & 0x69u) == 0) {
        ps_publish_done(self);
        return;
    }

    /* From STANDBY only continue if the substate marks a pending turn-on. */
    if (ps_target_state(self) == 0 && PS_substate(self) != 1) {
        ps_publish_done(self);
        return;
    }

    pc = PS_power_control(self);
    substate = ((uint8_t *)self)[0x121];   /* turn-on sub-flag (1=clear-flags req) */

    if (substate == 1) {
        bool battery_on;

        /* ---- attempt 1: clear flags + turn on ---- */
        common_logf(PSVC_CPP, 0xa6, LOG_INFO, "Try to clear battery flags.");
        /* OEM issues the CAN "clear flags" request to the battery here. */
        battery_cmd9_clear_fault(pc);
        ps_sleep_ns(2, 0);

        common_logf(PSVC_CPP, 0xad, LOG_INFO, "Try to turn battery on again.");
        battery_cmd1_on(pc);
        ps_sleep_ns(14, 0);

        battery_on = battery_is_on(pc);
        if (!battery_on) {
            /* ---- attempt 2: full battery RESET, then turn on ---- */
            common_logf(PSVC_CPP, 0xb5, LOG_INFO, "Battery still off, resetting battery.");
            battery_cmd6_reset(pc);
            ps_sleep_ns(21, 0);
            battery_cmd1_on(pc);
            ps_sleep_ns(14, 0);

            if (!battery_is_on(pc)) {
                common_logf(PSVC_CPP, 0xc3, LOG_ERR, "Unable to turn on battery.");
                ps_publish_done(self);
                ((uint8_t *)self)[0x121] = 3;     /* turn-on failed */
                return;
            }
            common_logf(PSVC_CPP, 0xbf, LOG_INFO, "Battery reset succesfull, battery on.");
        } else {
            common_logf(PSVC_CPP, 0xb1, LOG_INFO, "Succesfully cleared flags, battery on.");
        }
        ((uint8_t *)self)[0x121] = 2;             /* battery confirmed on */
    } else if (substate == 3 || substate == 0) {
        /* a prior failure (3) or nothing requested (0): nothing to do here. */
        ps_publish_done(self);
        return;
    }

    /* ---- status snapshot before BUCK bring-up ---- */
    common_logf(PSVC_CPP, 0xd0, LOG_INFO, "PGOOD: %d MuxSwitch: %d",
                bq25672_read_pgood(PS_lipo_gauge(self)),
                bq25672_read_muxswitch_tps2121(PS_lipo_gauge(self)));
    lipo_log_status(self, 0xd1, 0xd3, 0xd4, 0xd5);

    /* ---- BUCK + PGOOD retry (only while voltage is stable) ---- */
    if (battery_is_voltage_stable(pc)) {
        for (attempt = 1; ; attempt++) {
            common_logf(PSVC_CPP, 0xd9, LOG_INFO, "Enable Buck");
            buck_set_enable(PS_switch_ctrl(self), true);
            ps_sleep_ns(0, 100000000);            /* 100 ms */

            if (bq25672_read_pgood(PS_lipo_gauge(self))) {
                common_logf(PSVC_CPP, 0xdd, LOG_INFO, "PGOOD looks OK, moving on");
                break;
            }
            common_logf(PSVC_CPP, 0xe0, LOG_INFO,
                        "No PGOOD received after attempt: %d, turning BUCK off again", attempt);
            buck_set_enable(PS_switch_ctrl(self), false);
            ps_sleep_ns(0, 100000000);

            if (attempt == 5) {
                common_logf(PSVC_CPP, 0xe4, LOG_INFO, "Can't turn the buck ON");
                StateManager_ChangeState(self, ST_STANDBY);
                ps_publish_done(self);
                /* re-arm the 4000 ms tick (PS_tick_timer vtable+0x20, 3000 ms). */
                ps_timer_arm(PS_tick_timer(self), 3000);
                break;
            }
        }
    }

    /* ---- commit: VAC1 present + voltage stable -> flip switches, change state ---- */
    if (bq25672_read_vac1_chrgstat0(PS_low_power(self)) && battery_is_voltage_stable(pc)) {
        int next = ps_target_state(self);
        char name[64];
        uint64_t t_on, t_done;

        ps_set_switches(PS_switch_ctrl(self), next);
        ps_sleep_ns(0, 100000000);

        next = ps_target_state(self);
        StateManager_ChangeState(self, next);

        t_done = ps_now_ns();
        ((uint64_t *)self)[0x378 / 8] = t_done;

        common_logf(PSVC_CPP, 0xf4, LOG_INFO, "TurnOnCmd to TurnedOn in %f seconds",
                    (double)(((uint64_t *)self)[0x370 / 8] -
                             ((uint64_t *)self)[0x368 / 8]) / 1e9);

        StateManager_StateName(name, ps_target_state(self));
        common_logf(PSVC_CPP, 0xf6, LOG_INFO, "TurnedOn to %s in %f seconds",
                    name, (double)(((uint64_t *)self)[0x378 / 8] -
                                   ((uint64_t *)self)[0x370 / 8]) / 1e9);
        (void)t_on;

        common_logf(PSVC_CPP, 0xf8, LOG_INFO, "System Voltage: %d PackVoltage: %d Stable: %d",
                    battery_sys_voltage(pc), monitor_pack_voltage(PS_monitor(self)),
                    battery_is_voltage_stable(pc));
        common_logf(PSVC_CPP, 0xfa, LOG_INFO, "PGOOD: %d MuxSwitch: %d",
                    bq25672_read_pgood(PS_lipo_gauge(self)),
                    bq25672_read_muxswitch_tps2121(PS_lipo_gauge(self)));
        lipo_log_status(self, 0xf8, 0xfb, 0xfc, 0xfd);

        ps_publish_done(self);
    }
}
