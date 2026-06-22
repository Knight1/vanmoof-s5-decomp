/*
 * ux_common.h — canonical framework model for the reconstructed VanMoof S5
 * i.MX8 `ux` service (UX orchestrator). Behaviour-oriented C (C++ modelled in
 * C), matching the convention in main/update + main/power + main/tracking.
 *
 * Every reconstructed TU targets THIS model (do not invent parallel type names).
 * The framework — common::IMQTTClient, common::StateClient, nlohmann::json,
 * std::thread/mutex/cv/deque/function, the bike-VM/CAN bus, the sound/LED/light
 * drivers, the storage backend — is VENDOR: modelled as opaque handles + extern
 * prototypes, not reconstructed. OEM addresses are quoted in the .c files
 * (program "ux", AArch64, image base 0x100000).
 */
#ifndef UX_COMMON_H
#define UX_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common::logf (verified anchor 0x1a9590): 1=DBG 2=INFO 3=WARN 4=ERR --- */
enum log_level { LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4, LOG_ERR = 4 };
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* ---- UX state enum (UXService+0x1090) ----------------------------------- */
enum ux_state {
    UX_SHIPPING    = 1,
    UX_STANDBY     = 2,
    UX_OPERATIONAL = 3,
    UX_CHARGING    = 4,
    UX_UPDATING    = 5,
    UX_ALARM       = 6,
    UX_MAINTENANCE = 7
};

/* ---- vendor framework: opaque handles ----------------------------------- */
typedef struct mqtt_client mqtt_client;       /* common::IMQTTClient */
typedef struct ux_service  ux_service;        /* the UXService context (passed to strategies) */
typedef struct json_t { void *_opaque; } json_t; /* nlohmann::json (complete: stack locals) */

/* ---- MQTT surface (IMQTTClient vtable) ----------------------------------- */
typedef void (*mqtt_handler)(void *ctx, const char *topic, const json_t *payload);
void mqtt_subscribe   (mqtt_client *c, const char *topic, mqtt_handler fn, void *ctx);
void mqtt_unsubscribe (mqtt_client *c, const char *topic);
void mqtt_publish_json(mqtt_client *c, const char *topic, const json_t *body, int qos, int retain);
void mqtt_publish_bool(mqtt_client *c, const char *topic, bool v, int qos, int retain);
void mqtt_publish_int (mqtt_client *c, const char *topic, long v, int qos, int retain);
void mqtt_publish_str (mqtt_client *c, const char *topic, const char *v, int qos, int retain);
void mqtt_flush       (mqtt_client *c);

/* ---- nlohmann::json accessors (modelled at the call site) ---------------- */
bool        json_get_bool  (const json_t *j, bool *out);
bool        json_get_int   (const json_t *j, int *out);
bool        json_get_double (const json_t *j, double *out);
const char *json_get_string(const json_t *j);
bool        json_find      (const json_t *j, const char *key, json_t *out);
void        json_free      (json_t *j);
char       *json_dump      (const json_t *j);
void        json_dump_free (char *s);

/* ---- operator new / delete + timers (modelled) -------------------------- */
void *op_new(size_t n);
void  op_delete(void *p, size_t n);
typedef void (*ux_fn)(void);  /* generic callback (type-erased registration) */
typedef void (*ux_timer_cb)(void *ctx);
typedef struct ux_timer ux_timer;
ux_timer *ux_timer_new(ux_service *ctx);
void ux_timer_arm_periodic(ux_timer *t, ux_timer_cb cb, void *ctx, unsigned ms);
void ux_timer_arm_oneshot (ux_timer *t, ux_timer_cb cb, void *ctx, unsigned ms);
void ux_timer_stop(ux_timer *t);

/* ---- UXService subsystem accessors (the FUN_0013db* context thunks) ------ *
 * Each returns the sub-manager pointer the strategies act through.          */
mqtt_client  *ux_mqtt   (ux_service *ctx);      /* MQTT publisher (db_c0) */
void         *ux_sound  (ux_service *ctx);      /* sound/LED-anim mgr (db_80) */
void         *ux_light  (ux_service *ctx);      /* light mgr (db_d0) */
void         *ux_lock   (ux_service *ctx);      /* elock/lock mgr (db_60/db_90) */
void         *ux_ride   (ux_service *ctx);      /* ride/motor mgr (db_70) */
void         *ux_bike   (ux_service *ctx);      /* bike-state mgr (db_e0) */
void         *ux_ble    (ux_service *ctx);      /* BLE mgr (db_f0) */
void         *ux_sensor (ux_service *ctx);      /* sensor mgr (db_a0) */

/* ---- common UX effects used across modules ------------------------------ */
void ux_sound_play (ux_service *ctx, const char *name);           /* publish ux/sound/play */
void ux_light_pattern(ux_service *ctx, int pattern_id, int mode); /* start LED-ring pattern */
void ux_light_set_mode(ux_service *ctx, int mode);
bool ux_is_theft(ux_service *ctx);                                /* kTheft gate */
bool ux_is_shipping(ux_service *ctx);

/* ---- state transitions (UXService::To<State> virtuals) ------------------ */
void ux_to_shipping   (ux_service *svc);   /* 0x12dbb0 */
void ux_to_standby    (ux_service *svc);   /* 0x12dc70 */
void ux_to_operational(ux_service *svc);   /* 0x12dd30 */
void ux_to_charging   (ux_service *svc);   /* 0x12ddf0 */
void ux_to_updating   (ux_service *svc);   /* 0x12deb0 */
void ux_to_alarm      (ux_service *svc);   /* 0x12df70 */
void ux_to_maintenance(ux_service *svc);   /* 0x12e030 */
void ux_on_state_entered(ux_service *svc, int new_state); /* 0x13ef20 persist hook */

#endif /* UX_COMMON_H */
