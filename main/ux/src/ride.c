/*
 * ride.c — RideService logic for the VanMoof S5 i.MX8 `ux` service.
 * Behaviour reconstruction of devices/main/ux/src/ride.cpp.
 * Program "ux" (AArch64, image base 0x100000). OEM addresses in comments.
 *
 * The std::function listener fan-out, nlohmann::json variant teardown, and
 * std::string churn that dominate the decompiler output are VENDOR glue; they
 * are modelled at the call site (ride_listeners_fire / ride_json_to_* /
 * ride_mqtt_publish / ride_storage_*) and not reconstructed here.
 */
#include "ux_common.h"
#include "ride.h"

/* +0x74 seed (OEM 64-bit store param_1[0xe] = 0x6400000000 -> +0x74 = 100). The
 * OEM reuses this single field for two roles: the per-update delta sanity-cap
 * (ride_update_distance line 0x57 warning) AND the unsaved-distance persist
 * threshold (storage write gate). */
#define RIDE_DISTANCE_STEP_LIMIT 0x64u

struct ride_service {
    void    *vtable;        /* +0x00  PTR_FUN_001fa3f8 */
    /* listener bookkeeping (+0x08 mutex, +0x38 list) lives in vendor base */
    unsigned motor_distance_base;  /* +0x70 */
    unsigned distance_step_limit;  /* +0x74 */
    char     _pad0[0x88 - 0x78];
    void    *storage_key_distance; /* +0x88  "distance" */
    char     _pad1[0x98 - 0x90];
    unsigned trip_distance;        /* +0x98 */
    unsigned motor_distance_prev;  /* +0x9c */
    unsigned distance_persisted;   /* +0xa0 */
    mqtt_client *mqtt;             /* +0xa8 */
    void    *storage;             /* +0xb0 */
};

/* ---- ride_ctor @0x155310 -------------------------------------------------
 * Builds the RideService vtable (PTR_FUN_001fa3f8), zeroes the listener list,
 * seeds the per-update step limit (0x64 == 100) and the initial reading base,
 * subscribes the four ride MQTT topics, and registers the persisted-distance
 * default with the storage manager (key "distance").
 */
void ride_ctor(ride_service *self, mqtt_client **mqtt, void *storage)
{
    self->motor_distance_base = 0;
    self->distance_step_limit = RIDE_DISTANCE_STEP_LIMIT;
    self->trip_distance       = 0;
    self->motor_distance_prev = 0;
    self->distance_persisted  = 0;
    self->storage_key_distance = (void *)"distance";
    self->mqtt    = (mqtt_client *)mqtt;
    self->storage = storage;

    /* IMQTTClient::subscribe(topic, handler, this) — vtable slot +0x10. */
    mqtt_subscribe(self->mqtt, "ride/info/speed",    (mqtt_handler)(ux_fn)ride_on_speed_msg,       self);
    mqtt_subscribe(self->mqtt, "ride/info/power",    (mqtt_handler)(ux_fn)ride_on_power_msg,       self);
    mqtt_subscribe(self->mqtt, "ride/info/distance", (mqtt_handler)(ux_fn)ride_on_distance_msg,    self);
    mqtt_subscribe(self->mqtt, "ride/brake_level",   (mqtt_handler)(ux_fn)ride_on_brake_level_msg, self);

    /* storage_manager::register_default(key, idx, ctx, cb) for distance. */
    ride_storage_register_default(storage, "distance", 2, self,
                                  (ux_fn)ride_restore_distance);
}

/* ---- ride_update_distance @0x156450 --------------------------------------
 * Validates a new motor odometer reading against the previous one, accumulates
 * the trip total, publishes "distance", and persists it once the unsaved delta
 * exceeds the step limit.
 */
void ride_update_distance(ride_service *self, unsigned motor_distance)
{
    unsigned prev = self->motor_distance_prev;
    long delta;

    /* First reading after boot: latch base == prev. */
    if (self->motor_distance_prev == self->motor_distance_base) {
        self->motor_distance_prev = motor_distance;
        prev = motor_distance;
    }

    delta = (long)motor_distance - (long)prev;
    if (delta < 0) {
        common_logf("devices/main/ux/src/ride.cpp", 0x52, LOG_DEBUG,
                    "New motor distance=%d is smaller than the previous one=%d",
                    motor_distance, prev);
        return;
    }
    if (motor_distance != prev && (long)self->distance_step_limit < delta) {
        common_logf("devices/main/ux/src/ride.cpp", 0x57, LOG_INFO,
                    "New motor distance=%d is unexpectedly larger than the previous distance=%d",
                    motor_distance, prev);
    }

    self->motor_distance_prev = motor_distance;
    self->trip_distance       = (motor_distance + self->trip_distance) - prev;

    /* publish the running trip total on "distance". */
    ride_mqtt_publish(self->mqtt, "distance", (int)self->trip_distance);

    /* persist once enough unsaved distance has built up. */
    if ((self->trip_distance - self->distance_persisted) > self->distance_step_limit) {
        self->distance_persisted = self->trip_distance;
        ride_storage_set(self->storage, (const char *)self->storage_key_distance,
                         (int)self->trip_distance);
    }
}

/* ---- ride_restore_distance @0x156de0 -------------------------------------
 * Storage default-restore callback: if the stored json variant is an integral
 * type (tag in {5,6,7}), decodes it into the trip total.
 */
int ride_restore_distance(ride_service **self, const char *stored)
{
    int value = 0;

    if ((unsigned char)(*stored - 5) < 3) {     /* int / uint / float variant tags */
        value = ride_json_to_int((const json_t *)stored);
        (*self)->trip_distance = (unsigned)value;
        common_logf("devices/main/ux/src/ride.cpp", 0x2f, LOG_WARN,
                    "Restore distance to %d", (*self)->trip_distance);
    }
    return 1;
}

/* ---- ride_publish_boost @0x154a70 ----------------------------------------
 * Publishes the motor-boost state on "boost". (OEM: IMQTTClient::publish with
 * QoS arg 5 and a retain flag computed as (on ^ 1).)
 */
void ride_publish_boost(ride_service *self, unsigned char on)
{
    ride_mqtt_publish(self->mqtt, "boost", on);
    (void)on;
}

/* ---- ride_notify_speed @0x154340 -----------------------------------------
 * Fires the registered speed listeners (under the instance mutex) with the
 * latest km/h reading.
 */
void ride_notify_speed(ride_service *self, unsigned short kmh)
{
    unsigned short v = kmh;
    ride_listeners_fire(self, &v, (void *)0 /* FUN_00150f20 trampoline */);
}

/* ---- ride_notify_power @0x1544b0 -----------------------------------------
 * Fires the power listeners with the latest motor power (watts). NOTE the OEM
 * ABI passes (value, this) in that order.
 */
void ride_notify_power(int watts, ride_service *self)
{
    int v = watts;
    ride_listeners_fire(self, &v, (void *)0 /* FUN_00150f90 trampoline */);
}

/* ---- ride_notify_brake @0x1541d0 -----------------------------------------
 * Fires the brake-level listeners with the latest brake level byte.
 */
void ride_notify_brake(ride_service *self, unsigned char level)
{
    unsigned char v = level;
    ride_listeners_fire(self, &v, (void *)0 /* FUN_00151000 trampoline */);
}

/* ---- ride_on_speed_msg @0x156d00 -----------------------------------------
 * MQTT "ride/info/speed" handler: decodes a u16 km/h from the payload and
 * fans it out via ride_notify_speed.
 */
void ride_on_speed_msg(ride_service **self, const char *t, const char *p, const json_t *j)
{
    unsigned short kmh;
    (void)t; (void)p;
    kmh = ride_json_to_u16(j);
    ride_notify_speed(*self, kmh);
}

/* ---- ride_on_power_msg @0x156d70 -----------------------------------------
 * MQTT "ride/info/power" handler: decodes an int (watts) and notifies.
 */
void ride_on_power_msg(ride_service **self, const char *t, const char *p, const json_t *j)
{
    int watts;
    (void)t; (void)p;
    watts = ride_json_to_int(j);
    ride_notify_power(watts, *self);
}

/* ---- ride_on_distance_msg @0x156e90 --------------------------------------
 * MQTT "ride/info/distance" handler: decodes the motor odometer reading and
 * feeds ride_update_distance.
 */
void ride_on_distance_msg(ride_service **self, const char *t, const char *p, const json_t *j)
{
    int dist;
    (void)t; (void)p;
    dist = ride_json_to_int(j);
    ride_update_distance(*self, (unsigned)dist);
}

/* ---- ride_on_brake_level_msg @0x156f00 -----------------------------------
 * MQTT "ride/brake_level" handler: decodes a byte brake level and notifies.
 */
void ride_on_brake_level_msg(ride_service **self, const char *t, const char *p, const json_t *j)
{
    unsigned char level;
    (void)t; (void)p;
    level = ride_json_to_u8(j);
    ride_notify_brake(*self, level);
}
