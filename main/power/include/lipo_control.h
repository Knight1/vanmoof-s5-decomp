/*
 * lipo_control.h — LiPo (backup-cell) fuel gauge + charger control
 *
 * OEM: /usr/bin/power, devices/main/power/src/lipo_control.cpp (+ power_service.cpp
 *      for lipo_buck_enable_pgood, switch_control.cpp for buck_set_enable).
 *
 * Two TI parts behind this module:
 *   - bq27542  fuel gauge  -> /sys/class/power_supply/bq27542-0/   (read, publish)
 *   - BQ25672  buck/charger-> /sys/bus/i2c/devices/2-006b/{registers,gpios}
 *                             (register profiles, PGOOD/VAC1/MuxSwitch status)
 *   - BUCK enable is a GPIO on switch_control (PS_switch_ctrl, +0x208).
 *
 * The LiPo is the small on-board cell that keeps the bike alive while the main
 * (primary) pack is detached. lipo_control reads the gauge, publishes the
 * power/battery/lipo/info/ topics, programs the charger register profile for
 * the current power-state, and (lipo_buck_enable_pgood) brings the buck up with
 * a PGOOD retry loop and a battery fault-recovery path.
 */
#ifndef LIPO_CONTROL_H
#define LIPO_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "power_common.h"

/* sysfs roots (verbatim from the binary). */
#define BQ27542_SYSFS  "/sys/class/power_supply/bq27542-0"
#define BQ25672_SYSFS  "/sys/bus/i2c/devices/2-006b"

/* ---- bq27542 fuel-gauge single-attribute reads ----------------------- */
/* Each opens BQ27542_SYSFS/<attr>, fscanf("%d"), returns the value or -1. */
int lipo_gauge_read_charge_now(void);          /* OEM 0x1214c0  -> info/soc           */
int lipo_gauge_read_charge_full_design(void);  /* OEM 0x1215c0  -> charge_full_design */
int lipo_gauge_read_charge_full(void);         /* OEM 0x1216c0  -> charge_full        */
int lipo_gauge_read_capacity(void);            /* OEM 0x1213c0  -> capacity           */
int lipo_gauge_read_power_avg(void);           /* OEM 0x1217c0  -> pwr_avg            */
int lipo_gauge_read_voltage_now(void);         /* OEM 0x1218c0  -> voltage            */
int lipo_gauge_read_current_now(void);         /* OEM 0x1219c0  -> current_now        */
int lipo_gauge_read_cycle_count(void);         /* OEM 0x121ac0  -> cycles             */
int lipo_gauge_read_temp(void);                /* OEM 0x121bc0  -> temp (hwmon1)      */
/* string attributes; fscanf("%s") into *out (std::string), "" on failure. */
void lipo_gauge_read_health(char *out, size_t cap);          /* OEM 0x122590 */
void lipo_gauge_read_status(char *out, size_t cap);          /* OEM 0x1226d0 */
void lipo_gauge_read_capacity_level(char *out, size_t cap);  /* OEM 0x122450 */

/* OEM (publisher) — read every gauge attribute and publish the lipo/info topics. */
void lipo_publish_info(PowerService *self);

/* ---- BQ25672 charger sysfs status -------------------------------------- */
bool bq25672_read_pgood(void *lipo_gauge);              /* OEM 0x121e20 */
bool bq25672_read_muxswitch_tps2121(void *lipo_gauge);  /* OEM 0x121f80 */
/* registers/CHRG_STAT_0 bit1 (VAC1_PRESENT). object is PS_low_power (+0x3a0). */
bool bq25672_read_vac1_chrgstat0(void *low_power);      /* OEM 0x123f90 */
/* read register `reg` from 2-006b/registers; returns value byte or -1. */
int  bq25672_read_register(void *self, uint8_t reg);    /* OEM 0x122cb0 */

/* ---- BQ25672 register profiles ----------------------------------------- */
void lipo_bq25672_write_regs_shipping(void);  /* OEM 0x1220e0 */
void lipo_bq25672_write_regs_standby(void);   /* OEM 0x122290 */

/* ---- BUCK enable (switch_control GPIO) --------------------------------- */
void buck_set_enable(void *switch_ctrl, bool enable);   /* OEM 0x11d920 */

/* ---- bring-up: enable buck, wait for PGOOD, recover battery faults ----- */
void lipo_buck_enable_pgood(PowerService **self);       /* OEM 0x1136e0 */

#endif /* LIPO_CONTROL_H */
