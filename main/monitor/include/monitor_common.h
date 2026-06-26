/*
 * monitor_common.h — canonical framework model for the reconstructed VanMoof S5
 * i.MX8 `monitor` service (component health/version supervisor). Behaviour-
 * oriented C (C++ modelled in C), matching the other i.MX8 targets.
 *
 * The framework — common::IMQTTClient (mosquitto), the CANopen/CAN context, the
 * C++ runtime / nlohmann-json, std::thread/mutex — is VENDOR: modelled as opaque
 * handles + extern prototypes. OEM addresses are quoted in the .c files
 * (program "monitor", AArch64, image base 0x100000).
 *
 * IComponent ABI (the vtable slot map, ground-truthed from
 * monitor_print_component_row @0x121ac0):
 *   +0x30 get_name()    -> const char*     +0x38 get_status() -> const char*
 *   +0x40 get_version() -> const char*     +0x48 is_alive()   -> bool
 *   +0x50 get_value()   -> float (seconds) +0x58 get_type()   -> const char*
 *   +0x28 poll() — the per-tick supervise method (slot before get_name).
 */
#ifndef MONITOR_COMMON_H
#define MONITOR_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common::logf: 1=DBG 2=INFO 3=WARN 4=ERR ---------------------------- */
enum log_level { LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4, LOG_ERR = 4 };
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* ---- vendor framework: opaque handles ----------------------------------- */
typedef struct mqtt_client mqtt_client;   /* common::IMQTTClient (mosquitto) */
typedef struct can_bus     can_bus;       /* SocketCAN / vm context */

void  mqtt_publish_str(mqtt_client *c, const char *topic, const char *v, int qos, int retain);
void  mqtt_subscribe  (mqtt_client *c, const char *topic, void (*cb)(void *, const char *, const void *), void *ctx);
void *op_new(size_t n);
void  op_delete(void *p, size_t n);

/* ---- IComponent: the supervised-component interface --------------------- *
 * Each component (BLE / MotorController(LPC) / modem) is an IComponent subclass;
 * the supervisor drives it through these ops every tick.                     */
typedef struct icomponent icomponent;
typedef struct icomponent_ops {
    void        (*dtor)      (icomponent *self);
    const char *(*get_name)  (icomponent *self);   /* vt+0x30 */
    const char *(*get_status)(icomponent *self);   /* vt+0x38 */
    const char *(*get_version)(icomponent *self);  /* vt+0x40 */
    bool        (*is_alive)  (icomponent *self);   /* vt+0x48 */
    float       (*get_value) (icomponent *self);   /* vt+0x50 (seconds) */
    const char *(*get_type)  (icomponent *self);   /* vt+0x58 */
    void        (*poll)      (icomponent *self);   /* per-tick supervise */
} icomponent_ops;

struct icomponent {
    const icomponent_ops *ops;   /* +0x00 vtable (base &DAT_001707a0) */
    mqtt_client          *mqtt;  /* +0x08 mosquitto subobject (component_base_ctor) */
};

/* IComponent base ctor/dtor (component_base_ctor 0x146680 / dtor 0x146730):
 * installs the base vtable + a mosquitto handle; mosquitto glue is vendor. */
void icomponent_base_ctor(icomponent *self, mqtt_client *mqtt);
void icomponent_base_dtor(icomponent *self);

/* ---- GPIO reset lines (component.cpp + common/gpio.cpp) ----------------- *
 * sysfs /sys/class/gpio — export + direction + value. Real VanMoof code.    */
typedef struct gpio_reset_line {
    int pin;
    bool active_low;
} gpio_reset_line;
void gpio_export_and_configure(int pin, bool out);     /* 0x138bd0 */
void gpio_set_value(int pin, int value);               /* 0x139180 (1/0 = assert/deassert) */
void gpio_write_file(const char *path, const char *s); /* 0x139950 */
void gpio_reset_line_ctor(gpio_reset_line *l, int pin);/* 0x134bf0 */
/* the reset bank: 4 lines on GPIO pins 1, 0, 10, 11 (0x120310). */
#define MONITOR_RESET_PINS { 1, 0, 10, 11 }

/* CAN node-id -> ECU display name (0x12ceb0). */
const char *can_node_id_to_ecu_name(uint8_t node);   /* 0xA1=Motor, 0xC1=Elock, … */

#endif /* MONITOR_COMMON_H */
