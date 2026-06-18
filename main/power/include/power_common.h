/*
 * power_common.h — shared types/interfaces for the reconstructed `power` modules
 *
 * OEM: /usr/bin/power (devices/main/power + devices/main/common). The PowerService
 * object is offset-based in the binary; the reconstructed modules access it via
 * the documented offsets / accessor macros below. Behaviour-oriented C — not a
 * byte-identical rebuild (STL/vm/mosquitto framework is modelled, not rebuilt).
 */
#ifndef POWER_COMMON_H
#define POWER_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "vm_can.h"
#include "od_table.h"

/* ---- enums ----------------------------------------------------------- */

/* Battery/charger command opcodes: stamped into CAN frame[0] and sent to the
 * power-control board (node a0=0xA3, OD id 0x14603040), relayed to the battery. */
enum battery_cmd {
    BATT_CMD_OFF         = 0,
    BATT_CMD_ON          = 1,
    CHARGER_CMD_IDENTIFY = 5,
    BATT_CMD_RESET       = 6,
    BATT_CMD_SHIPPING    = 8,
    BATT_CMD_CLEAR_FAULT = 9,
};

/* Power states (StateManager). OEM enum order from StateManager_StateName 0x142670. */
enum power_state {
    ST_INVALID = 0, ST_SHIPPING, ST_STANDBY, ST_OPERATIONAL,
    ST_CHARGING, ST_UPDATING, ST_ALARM, ST_MAINTENANCE,
};

enum log_level { LOG_ERR = 1, LOG_WARN = 2, LOG_INFO = 3 };

/* ---- framework interfaces (modelled; provided by vm/common/mosquitto) ---- */

/* OEM common_logf @0x158f60: (file, line, level, fmt, ...). */
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* OD/MQTT publish — OEM ipc vtable+0x20 (topic, typed value, qos, retain). */
void od_pub_u16   (void *ipc, const char *topic, uint16_t v, int qos, int retain);
void od_pub_int   (void *ipc, const char *topic, long     v, int qos, int retain);
void od_pub_double(void *ipc, const char *topic, double   v, int qos, int retain);
void od_pub_float (void *ipc, const char *topic, float    v, int qos, int retain);
void od_pub_bool  (void *ipc, const char *topic, bool     v);

/* sysfs / gpio helpers (devices/main/common: sysfs_utils.h, gpio.cpp). */
long sysfs_read_long(const char *path);
int  sysfs_write_str(const char *path, const char *val);
bool gpio_get(int line);                 /* active-low aware */
void gpio_set(int line, bool value);

/* OEM 0x133590: stamp `opcode` into a CAN frame and send to the power-control
 * board (node 0xA3, OD id 0x14603040). */
void battery_send_command(void *power_control, uint8_t opcode);

/* vm OD write / override helpers (framework — modelled). */
int  od_begin_write(void *od_handle, uint8_t **buf_out);  /* OEM 0x138ec0 */
int  od_commit(void *od_handle);                          /* OEM 0x138f20 */
bool od_override_contains(const char *key);               /* OEM 0x13df10 */
void od_override_set(const char *key, bool v);            /* OEM 0x13eaa0 */
void od_override_set_bool(const char *key, int v);        /* OEM 0x13e220 */

/* Static name for a power_state (the OEM StateManager::StateName builds a
 * std::string into an out-param; this convenience returns the same literal). */
const char *power_state_name(enum power_state state);

/* Model the PowerService periodic-timer arm (OEM timer vtable+0x20). */
void ps_timer_arm(void *timer_obj, int ms);

/* ---- the PowerService object (0x448 bytes), accessed by OEM offset ----- */

typedef struct PowerService PowerService;
struct PowerService { uint8_t _raw[0x448]; };

#define PS_FIELD(p, off, ty)   (*(ty *)((uint8_t *)(p) + (off)))
#define PS_SUBOBJ(p, off)      ((void *)((uint8_t *)(p) + (off)))

#define PS_ipc(p)            PS_FIELD(p, 0x10, void *)   /* IMQTTClient / publish */
#define PS_state(p)          PS_FIELD(p, 0x40, uint32_t) /* current power_state   */
#define PS_power_control(p)  PS_SUBOBJ(p, 0x50)          /* battery/charger power */
#define PS_soc(p)            PS_FIELD(p, 0x80, uint16_t) /* primary raw SoC       */
#define PS_charge_current(p) PS_FIELD(p, 0x82, uint16_t)
#define PS_voltage(p)        PS_FIELD(p, 0x8c, uint16_t) /* primary pack mV       */
#define PS_mutex(p)          PS_SUBOBJ(p, 0xe8)          /* recursive mutex       */
#define PS_substate(p)       PS_FIELD(p, 0x120, uint8_t) /* standby sub-state     */
#define PS_lipo_gauge(p)     PS_SUBOBJ(p, 0x128)         /* bq27542 gauge object  */
#define PS_monitor(p)        PS_SUBOBJ(p, 0x150)         /* Monitor (telemetry)   */
#define PS_tick_timer(p)     PS_SUBOBJ(p, 0x1f0)         /* 4000 ms periodic      */
#define PS_fast_timer(p)     PS_SUBOBJ(p, 0x200)         /* 50 ms periodic        */
#define PS_switch_ctrl(p)    PS_SUBOBJ(p, 0x208)         /* switch_control / GPIO */
#define PS_low_power(p)      PS_SUBOBJ(p, 0x3a0)         /* low_power / CAN client*/
#define PS_lipo_check(p)     PS_FIELD(p, 0x394, uint32_t)/* LiPo charge-check cnt */

#endif /* POWER_COMMON_H */
