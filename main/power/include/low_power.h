/*
 * low_power.h — VanMoof `power` low-power / suspend / poweroff module
 *
 * OEM: /usr/bin/power (devices/main/power/src/low_power.cpp), AArch64 C++,
 * stripped. Image base 0x100000. Reconstructed from the decompiled logic;
 * behaviour-oriented C (the STL/vm/mosquitto framework is modelled, not rebuilt).
 *
 * The LowPower object is the PowerService sub-object at PS_low_power (+0x3a0).
 * It owns:
 *   +0x08  RTC handler (rtc_handler.cpp)          — fd at +0x08, ioctl wrappers
 *   +0x18  CAN bus client / "vcan suspend" control — vm CAN backend handle
 *   +0x20  RTC wake-alarm sub-object               — SetWakeAlarm / GetWakeAlarm
 *   +0x28  WakeOnMotion (IMU) handler (wake_on_motion_handler.cpp)
 *   +0x30  SuspendHandler ("14SuspendHandler")     — writes /sys/power
 *   +0x38  WakeOnMotion enable/disable sub-object
 * These nested offsets are relative to the LowPower base (PS+0x3a0).
 */
#ifndef LOW_POWER_H
#define LOW_POWER_H

#include <stdint.h>
#include <stdbool.h>
#include "power_common.h"

/* ---- LowPower object layout (relative to PS_low_power, OEM PS+0x3a0) ---- */

typedef struct LowPower LowPower;
struct LowPower {
    uint8_t _hdr[0x08];     /* +0x00 vtable / ipc back-ptr                    */
    int     rtc_fd;         /* +0x08 open("/dev/rtc…") fd (RTC ioctls)        */
    uint8_t _r0[0x0c];      /* +0x0c                                          */
    void   *can_ctrl;       /* +0x18 vcan suspend-control handle (FUN_0x138a40)*/
    void   *rtc_alarm;      /* +0x20 wake-alarm sub-object                    */
    uint8_t imu_state;      /* +0x28 WoM IMU state (2 == unknown, re-init)    */
    uint8_t _r1[0x07];      /* +0x29                                          */
    void   *suspend;        /* +0x30 SuspendHandler (writes /sys/power)       */
    void   *wom;            /* +0x38 WakeOnMotion enable/disable sub-object   */
};

/* SuspendSystem return / "wake source" codes (FUN_0x123c30). */
enum lp_wake {
    LP_WAKE_RTC    =  1,    /* woke from the RTC wake alarm   */
    LP_WAKE_MOTION =  0,    /* woke on IMU motion             */
    LP_WAKE_ERROR  = -1,    /* a suspend / wake step failed   */
};

/* ---- MQTT topics (od/IPC) --------------------------------------------- */
#define LP_TOPIC_LOW_POWER        "power/low_power"        /* pub */
#define LP_TOPIC_DEEP_SLEEP       "power/deep_sleep"       /* pub */
#define LP_TOPIC_LOW_POWER_EXTEND "power/low_power_extend" /* sub */

/* ---- public entry points ---------------------------------------------- */

/* OEM 0x123c30. The standby->sleep entry. `wake_alarm_s` is the RTC wake
 * timeout in seconds (caller passes 0x708 = 1800s); `enable_wom` arms IMU
 * wake-on-motion; `s2idle` selects shallow s2idle vs deep suspend. Returns an
 * enum lp_wake. */
int  low_power_suspend_system(LowPower *lp, int wake_alarm_s,
                              char enable_wom, uint8_t s2idle);

/* OEM 0x124660. Hard power-off: gated on no AC/VBUS voltage; writes "00 11" to
 * the LPC55 subcommandcode then runs "/sbin/poweroff". */
void low_power_poweroff(LowPower *lp);

/* OEM 0x124860. Broadcast a standby request to battery / LPC / nRF, then quiesce
 * the local CAN bus. Publishes power/low_power = 1. */
void low_power_request_standby(LowPower *lp);

/* OEM 0x125030. Re-broadcast standby (power/low_power = 2) — used by TurnOn. */
void low_power_request_standby2(LowPower *lp);

/* OEM 0x124c40. Publish power/deep_sleep = `going_to_sleep` (0/1). */
void low_power_publish_deep_sleep(LowPower *lp, bool going_to_sleep);

/* OEM 0x1242f0. Read BQ25672 CHRG_STAT_0 from sysfs registers; true if any of
 * the low 3 bits (VAC2/VAC1/VBUS present) is set. */
bool low_power_ac_present(LowPower *lp);

/* OEM 0x1239c0. Poll vcan0 rx_packets, sleep `timeout_ms`, re-read; returns
 * true if the count is unchanged (bus is quiet). */
bool low_power_can_bus_quiet(LowPower *lp, long timeout_ms);

/* OEM 0x1238c0 / 0x123940: suspend (mode 1) / stop (mode 2) the vcan device. */
void low_power_can_suspend(LowPower *lp);
void low_power_can_stop(LowPower *lp);

/* OEM 0x116280: power/low_power_extend subscriber. Resets the standby tick
 * timer to `seconds * 1000` ms. `f->data[0]` is the od_value type tag. */
void low_power_on_extend(PowerService **ps, void *a1, void *a2,
                         const struct vm_frame *f);

#endif /* LOW_POWER_H */
