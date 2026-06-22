/*
 * state_operational.h - StateOperationalStrategy object + call-site model for
 * devices/main/ux/src/state_operational_strategy.cpp (program "ux", base
 * 0x100000). Included after ux_common.h.
 *
 * The OEM strategy object is 0x440 bytes; only the fields the reconstructed
 * functions touch are modelled. The vendor std::function / std::unordered_map
 * registration machinery is reduced to the so_* helper externs below (call-site
 * model only - not reconstructed).
 */
#ifndef STATE_OPERATIONAL_H
#define STATE_OPERATIONAL_H

#include "ux_common.h"

typedef struct state_operational_strategy state_operational_strategy;

struct state_operational_strategy {
    int          state_id;             /* +0x08 entered-from state id */
    ux_service  *svc;                  /* +0x30 UXService context (param_1[6]) */
    void        *state_observer;       /* +0x38 bike-VM state observer (vendor) */
    void        *ride_obs;             /* ride-manager observer slot (vendor) */
    void        *sensor_obs;           /* ble/sensor observer slot (vendor) */
    void        *lis_timeout;          /* +0x1a idle-timeout listener slot */
    void        *lis_tick;             /* +0x30 idle-tick listener slot */
    void        *lis_lock;             /* +0x72 lock-request listener slot */
    ux_timer    *idle_timeout_timer;   /* +0xd0 idle-timeout (1000ms / re-armed 3000ms) */
    ux_timer    *idle_anim_timer;      /* +0x180 idle-animation (8000ms) */
    int          idle_tick_count;      /* +0xbc tick gate (>0x32 gives up) */
    int          idle_active;          /* +0xc4 */
    long         ride_state;           /* +0xc8 / param_1[0x19] ride latch enum */
    int          prev_ride_state;      /* +0xcc carried-over latch */
    int          ride_latch;           /* +0x17 ride latch (-1 = idle) */
};

/* the context object passed to each registered handler: its first word points
 * back at the owning strategy (OEM: *param_1 == strategy, *(strategy+0x30) == svc). */
struct state_operational_evt {
    state_operational_strategy *strategy;
};

/* ---- handlers (OEM-named, exported) ------------------------------------- */
void state_operational_strategy_ctor(state_operational_strategy *self,
                                     int state_id, ux_service *svc);
void state_operational_on_lock_request(struct state_operational_evt *evt);   /* 0x1302f0 */
void state_operational_on_unlock(struct state_operational_evt *evt);         /* 0x1303b0 */
void state_operational_motor_assist_on(struct state_operational_evt *evt);   /* 0x130370 */
void state_operational_motor_assist_off(struct state_operational_evt *evt);  /* 0x130390 */
void state_operational_apply_animation_theme(struct state_operational_evt *evt); /* 0x130f30 */
void state_operational_on_shutdown(struct state_operational_evt *evt);       /* 0x131890 */
void state_operational_idle_animation_tick(struct state_operational_evt *evt); /* 0x1317d0 */
void state_operational_idle_timeout(struct state_operational_evt *evt);      /* 0x1312d0 */

/* ---- decoded variant blobs (nlohmann/std::vector, complete: stack locals) - */
typedef struct anim_theme_t  { void *_opaque; } anim_theme_t;
typedef struct light_state_t { void *_opaque; } light_state_t;

/* ---- subsystem accessors beyond ux_common.h (verified OEM offsets) ------- */
void *ux_bike_states(ux_service *svc);   /* +0x750 db80 sound/anim+state mgr */
void *ux_ride_flag(ux_service *svc);     /* +0xc60 dbc0 ride-flag mgr */
void *ux_light_store(ux_service *svc);   /* +0xc00 dba0 light-state store */
void *ux_alarm(ux_service *svc);         /* +0x978 dbd0 alarm mgr */
void *ux_power(ux_service *svc);         /* +0xb90 dbb0 power/shutdown mgr */

/* ---- sub-manager call-site helpers (other module groups / vendor glue) --- */
void ride_publish_boost(void *ride, int on);
void light_restore(void *light);
bool elock_is_unlocked(void *lock);
void elock_request_unlock(void *lock);
bool elock_unlock_suppressed(void *lock);  /* lockmgr+0x1022 (FUN_0013dc50) */
bool elock_ride_active(void *ride_flag);   /* ridemgr+0x101c==1 (FUN_0013dc20) */
void ride_clear_latch(void *ride_flag);    /* ridemgr+0xd0=0 (FUN_00198750) */
bool ride_flag_pending(void *ride_flag);   /* FUN_00198780 */
void ride_flag_clear(void *ride_flag);     /* FUN_00198750 */
void alarm_set_state(void *alarm, int s);
void bike_request_standby(void *bike);     /* FUN_0013e2a0: publish state=2 */
bool anim_theme_overridden(void *anim);    /* animmgr+0x1020 (FUN_0013dc30) */
void anim_set_active(void *anim, int on);  /* FUN_00147b00 */
void led_ring_play(void *anim, int ring, int id, int palette,
                   int p4, int repeat, int flag);   /* FUN_00147cd0 */
void anim_theme_get(void *anim, anim_theme_t *out); /* FUN_0013e340 */
void anim_theme_apply(anim_theme_t *t);
void anim_theme_release(anim_theme_t *t);
void light_state_get(void *store, light_state_t *out); /* FUN_00167030 */
void light_state_release(light_state_t *ls);
void power_shutdown(void *power);          /* FUN_0013dbb0 chain */
void ux_global_teardown(void);             /* FUN_00138e50 */

/* ux_timer wrappers over the OEM timer ops (FUN_00186760/00186fa0/001870c0) */
void ux_timer_rearm(ux_timer *t);          /* FUN_00186760 */

/* ---- vtable / observer / listener / handler registration (VENDOR glue) ---
 * std::function + std::unordered_map machinery; modelled at the call site. */
void so_install_vtables(state_operational_strategy *self);
void so_observer_register(void *slot, void *bus);              /* FUN_00148fa0 */
void so_listener_slot_init(void *slot);                       /* FUN_001869f0 */
void so_timer_init(ux_timer *t, state_operational_strategy *self, unsigned ms); /* FUN_00186ab0 */
void so_pre_enter(state_operational_strategy *self);          /* FUN_00181460 */
void ride_observer_register(void *ride, void *slot);          /* FUN_00160b80 */
void so_sensor_observer_register(void *sensor, void *slot);
void so_arm_idle_timeout(void *slot, unsigned ms);            /* FUN_001870c0 */
typedef void (*so_listener_fn)(struct state_operational_evt *);
void so_listener_register(void *slot, state_operational_strategy *self, so_listener_fn fn); /* FUN_00186da0 */
typedef void (*so_handler_fn)(struct state_operational_evt *);
void so_handler_on_enter(void *obs, state_operational_strategy *self, so_handler_fn fn);      /* FUN_001482a0 */
void so_handler_register(void *obs, int cls, int sub,
                         state_operational_strategy *self, so_handler_fn fn);                 /* FUN_00148dc0 */
void state_op_dispatch_ride(state_operational_strategy *self);   /* vtable+0x60 */
void state_op_dispatch_lock(state_operational_strategy *self, int arg); /* vtable+0x58 */

/* handlers owned by sibling module groups (state_charging / interior LABs) */
void state_charging_publish_unplugged(struct state_operational_evt *evt);
void state_charging_on_charge_stop(struct state_operational_evt *evt);
void so_handler_lab_130680(struct state_operational_evt *evt);
void so_handler_lab_130860(struct state_operational_evt *evt);
void so_handler_lab_132f70(struct state_operational_evt *evt);
void so_handler_lab_133120(struct state_operational_evt *evt);
void so_handler_lab_132d90(struct state_operational_evt *evt);
void so_handler_lab_132db0(struct state_operational_evt *evt);

#endif /* STATE_OPERATIONAL_H */
