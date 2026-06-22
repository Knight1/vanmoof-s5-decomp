/*
 * ride.h — RideService (reconstructed). Models the UXService ride/motor manager
 * for the i.MX8 `ux` service. OEM C++: devices/main/ux/src/ride.cpp.
 *
 * Layout offsets are quoted from the OEM object (program "ux", base 0x100000):
 *   +0x08   per-instance mutex (notify listener list lock)
 *   +0x38   listener list head (speed/power/brake notify callbacks)
 *   +0x70   motor_distance_base   (uint, first motor reading seen)
 *   +0x74   distance_step_limit   (uint, expected per-update delta cap)
 *   +0x88   storage key string ("distance")
 *   +0x98   trip_distance         (uint, accumulated total)
 *   +0x9c   motor_distance_prev   (uint, previous motor reading)
 *   +0xa0   distance_persisted    (uint, value last written to storage)
 *   +0xa8   MQTT publisher (IMQTTClient*)
 *   +0xb0   storage manager handle
 */
#ifndef RIDE_H
#define RIDE_H

#include "ux_common.h"

typedef struct ride_service ride_service;

/* OEM ABI surface (the .c keeps the OEM names + addresses). */
void ride_ctor(ride_service *self, mqtt_client **mqtt, void *storage);
void ride_update_distance(ride_service *self, unsigned motor_distance); /* 0x156450 */
int  ride_restore_distance(ride_service **self, const char *stored);    /* 0x156de0 */
void ride_publish_boost(ride_service *self, unsigned char on);          /* 0x154a70 */
void ride_notify_speed(ride_service *self, unsigned short kmh);         /* 0x154340 */
void ride_notify_power(int watts, ride_service *self);                  /* 0x1544b0 */
void ride_notify_brake(ride_service *self, unsigned char level);        /* 0x1541d0 */

void ride_on_speed_msg      (ride_service **self, const char *t, const char *p, const json_t *j);
void ride_on_power_msg      (ride_service **self, const char *t, const char *p, const json_t *j);
void ride_on_distance_msg   (ride_service **self, const char *t, const char *p, const json_t *j);
void ride_on_brake_level_msg(ride_service **self, const char *t, const char *p, const json_t *j);

/* ---- vendor framework helpers modelled at the call site ------------------ */
/* IMQTTClient::publish(topic, json_value) — vtable slot +0x20 (FUN @lock+0x20). */
extern void ride_mqtt_publish(mqtt_client *c, const char *topic, int value);
/* common::StateClient / persistent-storage set + register-default. */
extern void ride_storage_set(void *storage, const char *key, int value);
extern void ride_storage_register_default(void *storage, const char *key, int idx,
                                          void *cb_ctx,  ux_fn cb);
/* json -> primitive extractors (FUN_00144b20/00157f40/00158180/00144780). */
extern unsigned short ride_json_to_u16(const json_t *j);
extern int            ride_json_to_int(const json_t *j);
extern unsigned char  ride_json_to_u8 (const json_t *j);
/* notify a registered std::function listener list under the instance mutex. */
extern void ride_listeners_fire(ride_service *self, void *value, void *cb);

#endif /* RIDE_H */