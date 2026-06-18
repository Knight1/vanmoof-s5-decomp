/*
 * battery_decode.h — primary (Panasonic) battery OD payload decoders
 *
 * OEM: /usr/bin/power, monitor.cpp. Each OD signal registered by Monitor has a
 * decode->publish callback (the std::function target) that takes the 8 raw CAN
 * data bytes and publishes to the `power/battery/primary/info/...` topics. See
 * main/docs/can-bus.md §4 / main/power/README.md.
 */
#ifndef BATTERY_DECODE_H
#define BATTERY_DECODE_H

#include <stdint.h>

/*
 * Cached battery fields on the Monitor/PowerService object (OEM offsets, on
 * `this`). The decoders cache a few values for cross-signal use (e.g. the
 * power calc needs voltage from the previous voltage frame).
 */
struct battery_monitor {
    void    *ipc;            /* this+0x10  the publish (MQTT/IPC) client */
    uint16_t soc;            /* this+0x80  raw state-of-charge           */
    uint16_t charge_current; /* this+0x82                                 */
    uint16_t voltage;        /* this+0x8c  pack voltage (mV)              */
};

/*
 * Publish sink — OEM `(*(ipc_vtable+0x20))(ipc, &topic, &value, qos, retain)`.
 * Declared extern so this TU compiles standalone; provided by the IPC layer.
 */
extern void od_pub_u16   (void *ipc, const char *topic, uint16_t v, int qos, int retain);
extern void od_pub_double(void *ipc, const char *topic, double   v, int qos, int retain);
extern void od_pub_float (void *ipc, const char *topic, float    v, int qos, int retain);

/* OEM 0x12cf80: raw SoC byte -> display SoC (piecewise-linear remap). */
uint8_t soc_to_soc_app(uint8_t raw_soc);

/* The per-signal decoders. `p` points at the 8 raw CAN payload bytes. */
void battery_voltage_decode    (struct battery_monitor *m, const uint8_t *p); /* 0x1285c0 */
void battery_charging_decode   (struct battery_monitor *m, const uint8_t *p); /* 0x128730 */
void battery_health_decode     (struct battery_monitor *m, const uint8_t *p); /* 0x1289c0 */
void battery_capacity_decode   (struct battery_monitor *m, const uint8_t *p); /* 0x12d150 */
void battery_temperature_decode(struct battery_monitor *m, const uint8_t *p); /* 0x12da90 */

#endif /* BATTERY_DECODE_H */
