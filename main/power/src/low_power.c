/*
 * low_power.c — VanMoof `power` low-power / suspend / poweroff (reconstructed)
 *
 * OEM: /usr/bin/power  (Ghidra program "power", image base 0x100000)
 *   low_power_suspend_system    0x123c30   (SuspendSystem state machine)
 *   suspend_handler_write       0x11c8f0   (writes /sys/power)
 *   wom_enable                  0x11fa70   (event_wom_enable = 1)
 *   wom_disable                 0x11f3f0   (event_wom_enable = 0)
 *   rtc_set_wake_alarm          0x11a430   (read time, +offset, arm RTC alarm)
 *   rtc_set_alarm_ioctl         0x11a3d0   (RTC_WKALM_SET = 0x4028700f)
 *   rtc_get_wake_alarm          0x11a370   (RTC_WKALM_RD  = 0x80287010)
 *   low_power_poweroff          0x124660
 *   low_power_request_standby   0x124860   (pub power/low_power = 1)
 *   low_power_request_standby2  0x125030   (pub power/low_power = 2)
 *   low_power_publish_deep_sleep 0x124c40  (pub power/deep_sleep = 0/1)
 *   low_power_ac_present        0x1242f0   (BQ25672 CHRG_STAT_0 & 7)
 *   low_power_can_bus_quiet     0x1239c0   (vcan0 rx_packets delta)
 *   low_power_can_suspend       0x1238c0   (vcan suspend, mode 1)
 *   low_power_can_stop          0x123940   (vcan stop, mode 2)
 *   low_power_on_extend         0x116280   (power/low_power_extend sub)
 *
 * Faithful translation of the decompiled logic: real sysfs paths, RTC ioctl
 * numbers, register parsing and CAN opcodes are reproduced verbatim. The C++
 * std::ofstream / std::string / od_value-variant scaffolding is modelled with
 * plain C (fopen/snprintf, od_pub_*) — behaviour-identical, not byte-identical.
 */
#define _GNU_SOURCE
#include "low_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/rtc.h>

#define LP_CPP "devices/main/power/src/low_power.cpp"

/* ---- sysfs / device paths (verbatim from rodata) ---------------------- */
#define SYS_POWER_DIR        "/sys/power"
#define SYS_POWER_MEM_SLEEP  SYS_POWER_DIR "/mem_sleep"   /* select deep / s2idle */
#define SYS_POWER_STATE      SYS_POWER_DIR "/state"       /* "mem" => suspend     */

/* LPC55 secure-MCU command mailbox (I2C bus 2, addr 0x55). */
#define LPC55_SUBCMD  "/sys/bus/i2c/devices/2-0055/subcommandcode"

/* TI BQ25672 charger register dump (I2C bus 2, addr 0x6b). */
#define BQ25672_REGISTERS  "/sys/bus/i2c/devices/2-006b/registers"

/* vcan0 statistics — used for the "is the CAN bus quiet?" check. */
#define VCAN0_STATS  "/sys/class/net/vcan0/statistics"

/* ---- small sysfs helpers (model the std::ofstream/ifstream usage) ----- */

/* write "<val>\n" to `path`; returns 0 ok, -errno on failure. */
static int lp_write_str(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -errno;
    int n = fprintf(f, "%s\n", val);     /* OEM fmt "%s\n" @0x15dd58 */
    fclose(f);
    return n < 0 ? -errno : 0;
}

/* write "<val>\n" (decimal) to `path`; OEM fmt "%d\n" @0x15dfc8. */
static int lp_write_int(const char *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -errno;
    int n = fprintf(f, "%d\n", val);
    fclose(f);
    return n < 0 ? -errno : 0;
}

/* nanosleep that restarts on EINTR — the OEM's clock_nanosleep retry loop. */
static void lp_sleep_ns(time_t sec, long nsec)
{
    struct timespec ts = { sec, nsec };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR)
        ;
}

/* ====================================================================== */
/* RTC wake alarm — rtc_handler.cpp                                        */
/* ====================================================================== */

/* OEM 0x11a3d0: arm the RTC wake alarm (RTC_WKALM_SET). */
static int rtc_set_alarm_ioctl(LowPower *lp, const struct rtc_wkalrm *alm)
{
    if (ioctl(lp->rtc_fd, RTC_WKALM_SET, alm) != 0) {   /* 0x4028700f */
        common_logf("devices/main/power/src/rtc_handler.cpp", 0x5a, LOG_ERR,
                    "RTC SetWakeAlarm error: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* OEM 0x11a370: read the pending RTC wake alarm (RTC_WKALM_RD). `enabled` gets
 * the alarm.enabled flag; non-zero => the wake was *not* the RTC. */
static int rtc_get_wake_alarm(LowPower *lp, struct rtc_wkalrm *out)
{
    if (ioctl(lp->rtc_fd, RTC_WKALM_RD, out) != 0) {    /* 0x80287010 */
        common_logf("devices/main/power/src/rtc_handler.cpp", 0x52, LOG_ERR,
                    "%s", strerror(errno));             /* &DAT_0015d8c0 = "%s" */
        return -1;
    }
    return 0;
}

/* OEM 0x11a430: read the current RTC time, add `offset_s`, arm the alarm. */
static int rtc_set_wake_alarm(LowPower *lp, int offset_s)
{
    struct rtc_time now;
    struct rtc_wkalrm alm;
    time_t t;

    if (ioctl(lp->rtc_fd, RTC_RD_TIME, &now) != 0)      /* FUN_0011a2b0 */
        return -1;

    /* rtc_tm -> time_t (timegm), add the offset, back to rtc_tm. */
    t = timegm(&(struct tm){
        .tm_sec  = now.tm_sec,  .tm_min  = now.tm_min,  .tm_hour = now.tm_hour,
        .tm_mday = now.tm_mday, .tm_mon  = now.tm_mon,  .tm_year = now.tm_year,
    });
    t += offset_s;

    {
        struct tm g;
        gmtime_r(&t, &g);
        memset(&alm, 0, sizeof(alm));
        alm.time.tm_sec  = g.tm_sec;  alm.time.tm_min  = g.tm_min;
        alm.time.tm_hour = g.tm_hour; alm.time.tm_mday = g.tm_mday;
        alm.time.tm_mon  = g.tm_mon;  alm.time.tm_year = g.tm_year;
    }
    alm.enabled = 1;            /* local_68 = 1 */
    alm.pending = 0;            /* local_67/local_65 = 0 */

    return rtc_set_alarm_ioctl(lp, &alm);
}

/* ====================================================================== */
/* Wake-on-motion (IMU) — wake_on_motion_handler.cpp                       */
/* ====================================================================== */

/* The IMU sysfs base path lives at wom+0x08; we model it with a fixed string
 * (the OEM stores the iio device dir there). */
static const char *wom_base_path(const void *wom)
{
    return *(const char *const *)((const uint8_t *)wom + 8);
}

/* OEM 0x11fa70: arm wake-on-motion — writes 1 to <iio>/event_wom_enable.
 * If the IMU state is "unknown" (2) it first re-initialises the device. */
static int wom_enable(LowPower *lp)
{
    char path[0x80];
    void *wom = lp->wom;

    if (lp->imu_state == 2) {           /* *(param_1+0x28) == 2 */
        common_logf("devices/main/power/src/wake_on_motion_handler.cpp", 0x34,
                    LOG_INFO, "IMU unknown, retry to initialize WakeOnMotion");
        /* FUN_0011f780 -> FUN_0011f500: probe + (re)configure the IMU */
    }

    if ((unsigned)snprintf(path, sizeof(path), "%s/%s",
                           wom_base_path(wom), "event_wom_enable") >= sizeof(path))
        return -1;
    return lp_write_int(path, 1);       /* "%d\n", 1 */
}

/* OEM 0x11f3f0: disarm wake-on-motion — writes 0 to <iio>/event_wom_enable. */
static int wom_disable(LowPower *lp)
{
    char path[0x80];
    if ((unsigned)snprintf(path, sizeof(path), "%s/%s",
                           wom_base_path(lp->wom), "event_wom_enable") >= sizeof(path))
        return -1;
    return lp_write_int(path, 0);       /* "%d\n", 0 */
}

/* ====================================================================== */
/* SuspendHandler — writes /sys/power                                      */
/* ====================================================================== */

/*
 * OEM 0x11c8f0. The actual kernel suspend:
 *   1. write "deep" (or "s2idle" if `s2idle`) to /sys/power/mem_sleep
 *   2. write "mem" to /sys/power/state  -> CPU suspends here until a wake event
 * Returns 0 on success, or a negative errno from whichever write failed.
 */
static int suspend_handler_write(uint8_t s2idle)
{
    const char *mode = s2idle ? "s2idle" : "deep";      /* @0x15dd40 / 0x15dd38 */
    char path[0x80];
    int rc;

    /* select the suspend mode */
    snprintf(path, sizeof(path), "%s/%s", SYS_POWER_DIR, "mem_sleep");
    rc = lp_write_str(path, mode);
    if (rc != 0)
        return rc == -ENOENT ? rc : -1;   /* OEM: open fail w/ errno!=0 -> -1 */

    /* enter suspend — blocks until a wake source fires */
    snprintf(path, sizeof(path), "%s/%s", SYS_POWER_DIR, "state");
    return lp_write_str(path, "mem");                    /* @0x15dd60 */
}

/* ====================================================================== */
/* CAN bus quiesce / quiet check                                           */
/* ====================================================================== */

/* OEM 0x138a40: write a mode byte to the vm CAN suspend-control object (a
 * polled OD signal: lock the entry, store the mode byte, push it). mode 1 =
 * suspend, mode 2 = stop. Returns 0 on success. The store/push lives in the vm
 * framework (vendor); modelled here as an external hook. */
int vcan_set_mode(void *can_ctrl, uint8_t mode);

/* OEM 0x1238c0: suspend the local vcan device (mode 1). */
void low_power_can_suspend(LowPower *lp)
{
    if (vcan_set_mode(lp->can_ctrl, 1) != 0)
        common_logf(LP_CPP, 0x22, LOG_ERR, "Unable to suspend CAN bus");
}

/* OEM 0x123940: stop the local vcan device (mode 2). */
void low_power_can_stop(LowPower *lp)
{
    if (vcan_set_mode(lp->can_ctrl, 2) != 0)
        common_logf(LP_CPP, 0x29, LOG_ERR, "Unable to suspend CAN bus");
}

/* read vcan0 rx_packets; returns -1 and *ok=false on parse/IO error. */
static long read_rx_packets(bool *ok)
{
    char path[0x80];
    long v = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", VCAN0_STATS, "rx_packets");
    f = fopen(path, "r");                                /* mode "r" @0x15e418 */
    if (!f) {
        *ok = (errno == 0);     /* OEM: only errno==0 is treated as "no error" */
        return -1;
    }
    if (fscanf(f, "%d", (int *)&v) == 1) {               /* "%d" @0x15e098 */
        fclose(f);
        *ok = true;
        return v;
    }
    fclose(f);
    *ok = false;
    return -1;
}

/*
 * OEM 0x1239c0. Sample vcan0 rx_packets, sleep `timeout_ms`, sample again;
 * return true if the count is unchanged (no traffic). A failed read throws
 * std::runtime_error("Failed reading rx packets") in the OEM — here we treat a
 * hard read error as "not quiet".
 */
bool low_power_can_bus_quiet(LowPower *lp, long timeout_ms)
{
    bool ok = false;
    long before, after;
    (void)lp;

    before = read_rx_packets(&ok);
    if (!ok)
        return false;

    if (timeout_ms > 0)
        lp_sleep_ns(timeout_ms / 1000, (timeout_ms % 1000) * 1000000L);

    after = read_rx_packets(&ok);
    if (!ok)
        return false;

    return before == after;     /* FUN_001092b0(local_a0 == iStack_9c) */
}

/* ====================================================================== */
/* AC-present check (BQ25672 CHRG_STAT_0)                                  */
/* ====================================================================== */

/*
 * OEM 0x1242f0. Read the BQ25672 register dump, find the "CHRG_STAT_0=" line,
 * strtol the hex value, and return true if any of the low 3 bits is set
 * (VAC2 / VAC1 / VBUS voltage present). The OEM iterates ifstream lines; we
 * scan the same way.
 */
bool low_power_ac_present(LowPower *lp)
{
    FILE *f;
    char line[256];
    bool present = false;
    (void)lp;

    f = fopen(BQ25672_REGISTERS, "r");
    if (!f)
        return false;

    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "CHRG_STAT_0");          /* @0x15eb85 */
        char *eq;
        if (!p)
            continue;
        eq = strchr(p, '=');                            /* find '=' (0x3d) */
        if (!eq)
            break;
        present = (strtol(eq + 1, NULL, 16) & 7) != 0;  /* low 3 bits */
        break;
    }
    fclose(f);
    return present;
}

/* ====================================================================== */
/* SuspendSystem state machine                                            */
/* ====================================================================== */

/*
 * OEM 0x123c30. Called from charge_supervisor_worker as
 *   low_power_suspend_system(lp, 0x708, 1, 0)
 * (arm a 1800 s RTC wake, enable wake-on-motion, deep suspend).
 *
 * Sequence:
 *   - arm the RTC wake alarm (offset = wake_alarm_s seconds)
 *   - arm wake-on-motion
 *   - confirm the CAN bus is quiet (<=500 ms poll); else "CanBus wake up again"
 *   - suspend the system (blocks)
 *   - on wake: read the RTC alarm flag and disable wake-on-motion to learn the
 *     wake source -> "Wake on motion" / "Wake from RTC"
 */
int low_power_suspend_system(LowPower *lp, int wake_alarm_s,
                             char enable_wom, uint8_t s2idle)
{
    struct rtc_wkalrm alm;
    bool quiet;

    /* --- arm RTC wake --- */
    if (wake_alarm_s != 0 && rtc_set_wake_alarm(lp, wake_alarm_s) != 0) {
        common_logf(LP_CPP, 0x37, LOG_INFO, "SetWakeAlarm Error");
        if (enable_wom == 0)
            goto check_can;                 /* fall through to the quiet check */
        goto arm_wom;
    }
    if (enable_wom == 0)
        goto check_can;

arm_wom:
    /* --- arm wake-on-motion --- */
    if (wom_enable(lp) != 0) {
        common_logf(LP_CPP, 0x3e, LOG_INFO, "EnableWakeOnMotion Error");
        quiet = low_power_can_bus_quiet(lp, 500);
        if (!quiet) {
            common_logf(LP_CPP, 0x49, LOG_INFO, "CanBus wake up again");
            goto wake_source;
        }
        goto do_suspend;
    }

check_can:
    /* --- confirm the bus is quiet before sleeping --- */
    quiet = low_power_can_bus_quiet(lp, 500);
    if (!quiet) {
        common_logf(LP_CPP, 0x49, LOG_INFO, "CanBus wake up again");
        goto wake_source;
    }

do_suspend:
    /* --- enter suspend (blocks until a wake source) --- */
    if (suspend_handler_write(s2idle) != 0) {
        common_logf(LP_CPP, 0x45, LOG_INFO, "SuspendSystem Error");
        return LP_WAKE_ERROR;
    }

wake_source:
    /* --- determine why we woke --- */
    memset(&alm, 0, sizeof(alm));
    if (wake_alarm_s != 0 && rtc_get_wake_alarm(lp, &alm) != 0) {
        common_logf(LP_CPP, 0x4e, LOG_INFO, "GetWakeAlarm Error");
        return LP_WAKE_ERROR;
    }
    if (enable_wom != 0 && wom_disable(lp) != 0) {
        common_logf(LP_CPP, 0x55, LOG_INFO, "DisableWakeOnMotion Error");
        return LP_WAKE_ERROR;
    }

    if (alm.enabled != 0) {             /* (char)local_30 != 0 */
        common_logf(LP_CPP, 0x5b, LOG_INFO, "Wake on motion");
        return LP_WAKE_MOTION;          /* 0 */
    }
    common_logf(LP_CPP, 0x5e, LOG_INFO, "Wake from RTC");
    return LP_WAKE_RTC;                 /* 1 */
}

/* ====================================================================== */
/* Hard power-off                                                         */
/* ====================================================================== */

/*
 * OEM 0x124660. Only powers off if no AC/VBUS voltage is present (else it would
 * immediately re-power). Writes "00 11" to the LPC55 subcommand mailbox (the
 * secure MCU's "system off" subcommand), then execs /sbin/poweroff. A failed
 * system() throws std::runtime_error("Unable to execute system power off").
 */
void low_power_poweroff(LowPower *lp)
{
    if (low_power_ac_present(lp)) {      /* FUN_001242f0(lp) != 0 */
        common_logf(LP_CPP, 0x9d, LOG_ERR, "Voltage on VAC2, VAC1 and VBUS present");
        return;
    }

    /* tell the LPC55 secure MCU to power the system off */
    lp_write_str(LPC55_SUBCMD, "00 11");

    if (system("/sbin/poweroff") != 0)
        common_logf(LP_CPP, 0, LOG_ERR, "Unable to execute system power off");
        /* OEM throws std::runtime_error("Unable to execute system power off") */
}

/* ====================================================================== */
/* MQTT / OD publishers + standby broadcast                               */
/* ====================================================================== */

/*
 * OEM 0x124860. "Send standby request to battery, LPC and nRF devices": publish
 * the od signal power/low_power = 1 (od_value type tag 5 == int, qos 1, retain
 * 1). The bulk of the OEM function is the destructor for the od_value variant.
 * The caller (charge_supervisor_worker) first calls this, then quiesces CAN.
 */
void low_power_request_standby(LowPower *lp)
{
    void *ipc = *(void **)((uint8_t *)lp + 0x28);   /* lp+0x28 -> ipc handle */
    od_pub_int(ipc, LP_TOPIC_LOW_POWER, 1, 1, 1);
}

/* OEM 0x125030. Re-publish power/low_power = 2 (used from PowerService_TurnOn). */
void low_power_request_standby2(LowPower *lp)
{
    void *ipc = *(void **)((uint8_t *)lp + 0x28);
    od_pub_int(ipc, LP_TOPIC_LOW_POWER, 2, 1, 1);
}

/* OEM 0x124c40. Publish power/deep_sleep = going_to_sleep (od type tag 4). */
void low_power_publish_deep_sleep(LowPower *lp, bool going_to_sleep)
{
    void *ipc = *(void **)((uint8_t *)lp + 0x28);
    od_pub_int(ipc, LP_TOPIC_DEEP_SLEEP, going_to_sleep ? 1 : 0, 1, 1);
}

/*
 * OEM 0x116280. Subscriber for power/low_power_extend (registered in
 * PowerService_ctor). The payload is an od_value variant whose type tag is
 * f->data[0]; the OEM accepts type tags in [5,7]. It reads the int payload
 * (extend seconds) and restarts the standby tick timer to seconds*1000 ms.
 *
 * `ps` is &PowerService* (the OEM passes `param_1` = the subscriber closure
 * whose [0] is the PowerService). The tick timer object is PS_tick_timer
 * (+0x1f0); start() is vtable+0x20.
 */
void low_power_on_extend(PowerService **ps, void *a1, void *a2,
                         const struct vm_frame *f)
{
    uint8_t type_tag = f->data[0];
    PowerService *p;
    void **timer;
    int seconds;
    (void)a1; (void)a2;

    if ((uint8_t)(type_tag - 5) >= 3)       /* accept tags 5,6,7 */
        return;

    /* od_value -> int (FUN_00118ee0 reads the variant's integer payload) */
    seconds = (int)f->data[1] | ((int)f->data[2] << 8) |
              ((int)f->data[3] << 16) | ((int)f->data[4] << 24);
    common_logf("devices/main/power/src/power_service.cpp", 0x56, LOG_INFO,
                "Extending standby timeout with %d seconds", seconds);

    /* restart the standby tick timer: timer->start(seconds * 1000 ms) */
    p = ps[0];
    timer = (void **)PS_tick_timer(p);          /* +0x1f0 */
    {
        void (**vt)(void *, unsigned long) = *timer;
        vt[4](timer, (unsigned long)seconds * 1000);  /* vtable+0x20 */
    }
}
