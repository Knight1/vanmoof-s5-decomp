/*
 * tracking_common.h — shared framework model for the reconstructed VanMoof S5
 * i.MX8 `tracking` service (anti-theft / location). Behaviour-oriented C
 * (C++ modelled in C), matching the convention in main/update + main/power.
 *
 * The framework objects — common::IMQTTClient, common::StateClient,
 * nlohmann::json, std::thread / mutex / condition_variable / deque /
 * function — are VENDOR. They are modelled here as opaque handles + extern
 * prototypes that stand in for the vtable slots / STL operations; their
 * internals are NOT reconstructed. OEM addresses are quoted at each call site
 * in the .c files (program "tracking", AArch64, image base 0x100000).
 */
#ifndef TRACKING_COMMON_H
#define TRACKING_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common::logf level enum (verified from common_logf, anchor) --------- *
 * 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR.                                          */
enum log_level {
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
    LOG_ERR   = 4
};
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* ---- tracking state machine --------------------------------------------- *
 * 0=OFF, 1=AUTO, 2=THEFT; anything else stringifies as "INVALID".           */
enum tracking_state {
    TRACKING_OFF   = 0,
    TRACKING_AUTO  = 1,
    TRACKING_THEFT = 2
};

/* ---- vendor framework: opaque handles ----------------------------------- */
typedef struct mqtt_client  mqtt_client;    /* common::IMQTTClient            */
typedef struct state_client state_client;   /* common::StateClient            */
/* json node modelled as a complete opaque-blob so it can be a stack local
 * (sms_message holds a "tracking" sub-value by copy). */
typedef struct json_value { void *_opaque; } json_value; /* nlohmann::json node */
/* per-module aliases used by the reconstructed TUs */
typedef mqtt_client IMQTTClient;
typedef mqtt_client IMqttClient;
typedef json_value  json_t;
typedef json_value  mqtt_msg;

/* ---- MQTT client surface (IMQTTClient vtable, modelled as plain calls) --- *
 * subscribe +0x10 / unsubscribe +0x18 / publish +0x20 / flush +0x40 /        *
 * request_cellular_fix +0xc8.                                                */
typedef void (*mqtt_handler)(void *ctx, const char *topic, void *meta,
                             const json_value *payload);
void mqtt_subscribe  (mqtt_client *c, const char *topic, mqtt_handler fn,
                      void *ctx, int qos);
void mqtt_unsubscribe(mqtt_client *c, const char *topic);
void mqtt_publish_int(mqtt_client *c, const char *topic, long value,
                      int qos, int retain);
void mqtt_publish_str(mqtt_client *c, const char *topic, const char *value,
                      int qos, int retain);
void mqtt_flush      (mqtt_client *c);
void mqtt_request_cellular_fix(mqtt_client *c);   /* vtable +0xc8 */
void mqtt_null_client_abort(void);                /* OEM FUN_001070b0 */

/* ---- StateClient surface ------------------------------------------------ */
int  state_client_get_state(state_client *sc);          /* vtable +0x18 */
void state_client_set_state(state_client *sc, int state);/* vtable +0x10 */
void state_client_start    (mqtt_client *c, void *arg);  /* publish-client +0x18 */

/* ---- nlohmann::json accessors (modelled at the call site) ---------------- */
bool        json_get_int   (const json_value *j, int *out);    /* type 5 int */
bool        json_get_number(const json_value *j, double *out); /* type 7 double */
const char *json_get_string(const json_value *j);              /* string node */
int         json_is_object (const json_value *j);
/* find object key; copies value into *out (json copy-ctor), 1 if present. */
int         json_find      (const json_value *j, const char *key, json_value *out);
int         json_equals_string(const json_value *j, const char *s);
/* json::dump(indent) -> heap C string; release with json_dump_free(). */
char       *json_dump      (const json_value *j, int indent);
void        json_dump_free (char *s);
/* release a json value obtained by copy (json_find out-param). */
void        json_free      (json_value *j);

/* ---- operator new / delete (modelled) ----------------------------------- */
void *op_new   (size_t n);
void  op_delete(void *p, size_t n);

/* ====================================================================== *
 *  TrackingApp — common::StateClient subclass, OEM size 0x160.            *
 * ====================================================================== */
typedef struct TrackingApp {
    mqtt_client  *mqtt;          /* +0x60 publish IMQTTClient */
    state_client *state;         /* +0x68 StateClient         */
    int           cmd_queue[64]; /* +0x70 std::deque<int>     */
    int           cmd_head;
    int           cmd_tail;
    void         *cmd_mutex;     /* +0xc0 */
    void         *cmd_cv;        /* +0x120 */
    void         *worker;        /* +0x150 std::thread */
    bool          worker_run;    /* +0x158 */
} TrackingApp;

/* command-deque / worker plumbing (std::deque<int> + mutex/cv/thread). */
void  cmd_mutex_lock  (TrackingApp *self);
void  cmd_mutex_unlock(TrackingApp *self);
void  cmd_queue_push  (TrackingApp *self, int code);   /* push_back + notify */
bool  cmd_queue_pop   (TrackingApp *self, int *code);  /* false if empty */
void  cmd_cv_notify   (TrackingApp *self);
void  cmd_cv_wait     (TrackingApp *self);
void *worker_spawn    (void (*entry)(TrackingApp *), TrackingApp *self);
void  worker_join     (void *worker);

/* StateClient base ctor + the deque/json-map teardown (vendor, modelled). */
void state_client_base_ctor(TrackingApp *self, mqtt_client *mqtt, state_client *state);
void tracking_app_base_dtor(TrackingApp *self);

/* tracking_service.cpp public entry points (OEM addrs in the .c). */
void        tracking_service_app_ctor(TrackingApp *self, mqtt_client *mqtt,
                                      state_client *state, void *service_env);
void        tracking_service_app_dtor(TrackingApp *self);
void        tracking_service_app_delete(TrackingApp *self);
void        tracking_service_set_state(TrackingApp *self, int new_state, int publish);
void        tracking_service_command_thread(TrackingApp *self);
void        tracking_service_play_alarm_sound(TrackingApp *self);
const char *tracking_state_to_string(int state);

/* sms_message.cpp */
int sms_message_parse(const json_value *msg);   /* "tracking": enabled=0/disabled=1/else -1 */

/* ====================================================================== *
 *  Movement — IMU motion state object, OEM size 216, vtable @0x156be8.    *
 * ====================================================================== */
enum movement_state {
    MOVEMENT_STATE_UNSET      = 0,
    MOVEMENT_STATE_MOVING     = 1,   /* kMoving     */
    MOVEMENT_STATE_STATIONARY = 2    /* Stationary  */
};
#define MOVEMENT_IMU_TOPIC "ux/tracking/alarm/imu/triggered"   /* str @0x13a318 */

typedef struct movement_listener_list movement_listener_list;
typedef void (*movement_listener_fn)(void *ctx, int state);
typedef struct movement_mutex { void *_opaque[5]; } movement_mutex;   /* 0x28 */
typedef struct periodic_timer periodic_timer;
typedef void (*timer_cb_t)(void *ctx);

typedef struct Movement {
    const void             *vtable;        /* +0x00 PTR_movement_dtor_00156be8 */
    movement_mutex          lock;          /* +0x08 guards the listener list   */
    movement_listener_list *listeners;     /* +0x38 std::deque<void(int)>      */
    enum movement_state     state;         /* +0xa0 MovementState              */
    IMqttClient            *mqtt;          /* IMQTTClient&                     */
    periodic_timer         *revert_timer;  /* 30-min no-motion -> Stationary   */
} Movement;

void movement_mutex_lock  (movement_mutex *m);
void movement_mutex_unlock(movement_mutex *m);
void movement_listener_list_copy(movement_listener_list *dst, movement_listener_list *src);
void movement_listener_list_free(movement_listener_list *list);
/* broadcast a state change to all registered listeners (models the OEM
 * std::deque<std::function<void(int)>> walk under the lock). */
void movement_listeners_notify(movement_listener_list *list, int state);
periodic_timer *timer_new(IMqttClient *mqtt);                          /* FUN_001074e0 */
void timer_arm(periodic_timer *t, timer_cb_t cb, void *ctx, unsigned interval_ms); /* FUN_00122e10 */
void timer_stop_free(periodic_timer *t);                               /* FUN_001227d0 */

void movement_ctor(Movement *self, IMqttClient *mqtt);   /* @0x111df0 */
void movement_dtor(Movement *self);                      /* @0x112380 */
void movement_set_moving(Movement *self);                /* @0x1121e0 */
void movement_set_stationary(Movement *self);            /* @0x112060 */

/* ====================================================================== *
 *  CellLocator — cellular-tower location poller. RTTI @0x13a151.          *
 * ====================================================================== */
typedef struct cl_mutex { void *_opaque[8]; } cl_mutex;
typedef struct cl_cond  { void *_opaque[8]; } cl_cond;
typedef void (*cl_movement_fn)(void *ctx);
typedef void (*thread_entry_t)(void *arg);

typedef struct CellLocator {
    IMQTTClient    *client;            /* +0x18 / +0xc0 (vtable +0xc8 fix req) */
    Movement       *movement;          /* +0x20 */
    cl_movement_fn  movement_cb;       /* +0x16 std::function */
    cl_cond         cond;              /* +0xa8 */
    cl_mutex        mutex;             /* +0xa8 (paired) */
    long            tracking_type;     /* +0xd0 packed token (modem/tracking/type/set) */
    long            poll_interval_min; /* +0xa0 minutes */
    void           *thread;            /* native thread handle */
    unsigned char   running;           /* +0x98 */
    unsigned char   stop_requested;    /* +0x88 */
} CellLocator;

void  thread_state_init(cl_cond *cond, cl_mutex *mutex);   /* FUN_00122a60 */
void  mutex_lock(cl_mutex *m);
void  mutex_unlock(cl_mutex *m);
void  cond_notify(cl_cond *c);                             /* FUN_001227d0 */
void  cond_wait(cl_cond *c, cl_mutex *m);                  /* FUN_001077e0 */
void  cond_wait_for(cl_cond *c, cl_mutex *m, long ms);     /* FUN_00122f10 */
void *thread_spawn(thread_entry_t entry, void *arg);

void cell_locator_ctor(CellLocator *self, IMQTTClient *mqtt, Movement *mvmt,
                       cl_movement_fn movement_cb);        /* @0x10af70 */
void cell_locator_on_location(void *ctx, const char *topic, void *meta, const json_t *payload); /* @0x10b410 */
void cell_locator_poll_thread(CellLocator *self);          /* @0x10b6d0 */

/* ====================================================================== *
 *  main.cpp — ServiceEnv plumbing (common/src/service_env.cpp, vendor).   *
 * ====================================================================== */
typedef struct ServiceEnv { unsigned char _opaque[64]; } ServiceEnv; /* env ctx+handle, by value */
void  service_env_ctor   (ServiceEnv *env);                  /* FUN_0011ec30 */
void  service_env_connect(ServiceEnv *env, const char *name);/* FUN_0011f0f0 */
void *service_env_mqtt   (ServiceEnv *env);                  /* FUN_0011e9f0 */
void *service_env_state  (ServiceEnv *env);                  /* FUN_0011ea00 */
void  service_env_run    (ServiceEnv *env);                  /* FUN_0011e9b0 */
void  service_env_destroy(ServiceEnv *env);                  /* FUN_0011e780 */
void  service_env_dtor   (ServiceEnv *env);                  /* FUN_0011e6f0 */
void *operator_new(size_t n);                                /* ::operator new */
void  tracking_app_run(void *handle);                        /* App::run (vtable+8) */

int tracking_main(void);                                     /* @0x107c60 */

#endif /* TRACKING_COMMON_H */
