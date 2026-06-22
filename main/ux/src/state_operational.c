/*
 * state_operational.c - StateOperationalStrategy for the VanMoof S5 i.MX8 `ux`
 * service (program "ux", AArch64, image base 0x100000).
 *
 * Reconstructed from the named OEM functions in
 * devices/main/ux/src/state_operational_strategy.cpp:
 *
 *   state_operational_strategy_ctor          0x131900  (on-enter ctor)
 *   state_operational_on_lock_request        0x1302f0
 *   state_operational_on_unlock              0x1303b0
 *   state_operational_motor_assist_on        0x130370
 *   state_operational_motor_assist_off       0x130390
 *   state_operational_apply_animation_theme  0x130f30
 *   state_operational_on_shutdown            0x131890
 *   state_operational_idle_animation_tick    0x1317d0
 *   state_operational_idle_timeout           0x1312d0
 *
 * This is the OPERATIONAL state: the bike is awake and ride-ready. The ctor
 * enables the ride/light/sound subsystems, wires up ~14 bike-VM event handlers
 * (lock/unlock requests, motor assist on/off, animation theme, charge/plug
 * events, shutdown), arms an idle-animation timer (1000ms) and an idle-timeout
 * timer (8000ms), restores the saved light state, and re-evaluates the elock.
 *
 * The compiler/STL glue named *_fn_manager_NN / *_map_rehash_* in the OEM image
 * is std::function / std::unordered_map machinery (VENDOR): modelled here only
 * at the call site via the registration helpers below, not reconstructed.
 */
#include "ux_common.h"
#include "state_operational.h"

/* ---- subsystem accessors over the UXService context (the FUN_0013db* thunks).
 * Offsets verified from the OEM image; mapped onto the canonical ux_*()
 * accessors where one exists, otherwise modelled as a local thunk.        ----
 *   +0x440 db50  bike-state mgr  (ux_bike)
 *   +0x4b8 db60  light mgr       (ux_light)
 *   +0x698 db70  ride/motor mgr  (ux_ride)
 *   +0x750 db80  sound/anim mgr  (ux_sound)
 *   +0xb90 dbb0  power/shutdown mgr
 *   +0xbf8 db90  elock mgr       (ux_lock)
 *   +0xc00 dba0  light-state store
 *   +0xc60 dbc0  ride-flag mgr
 *   +0x978 dbd0  alarm mgr
 *   +0xac0 dbe0  ble/sensor mgr
 */

/* state_operational_motor_assist_on - OEM 0x130370.
 * Bike-VM "assist" event -> publish boost=1 on the ride manager. */
void state_operational_motor_assist_on(struct state_operational_evt *evt)
{
    void *ride = ux_ride(evt->strategy->svc);
    ride_publish_boost(ride, 1);
}

/* state_operational_motor_assist_off - OEM 0x130390.
 * Bike-VM "coast" event -> publish boost=0 on the ride manager. */
void state_operational_motor_assist_off(struct state_operational_evt *evt)
{
    void *ride = ux_ride(evt->strategy->svc);
    ride_publish_boost(ride, 0);
}

/* state_operational_on_unlock - OEM 0x1303b0.
 * Unlock event: if the lock-suppress flag (lockmgr+0x1022) is set, ignore;
 * otherwise restore the saved light state. */
void state_operational_on_unlock(struct state_operational_evt *evt)
{
    ux_service *svc = evt->strategy->svc;

    if (elock_unlock_suppressed(ux_lock(svc)))
        return;

    light_restore(ux_light(svc));
}

/* state_operational_on_lock_request - OEM 0x1302f0.
 * Lock-request event: restore the light state, clear the ride latch
 * (ridemgr+0xd0 -> 0), then drive the state machine. If the ride-active flag
 * (ridemgr+0x101c == 1) is set, dispatch the "ride" virtual (vtable+0x60),
 * otherwise the "standby/lock" virtual (vtable+0x58). */
void state_operational_on_lock_request(struct state_operational_evt *evt)
{
    ux_service *svc = evt->strategy->svc;

    light_restore(ux_light(svc));
    ride_clear_latch(ux_ride_flag(svc));     /* ridemgr+0xd0 = 0 (FUN_00198750) */

    if (elock_ride_active(ux_ride_flag(svc)))
        state_op_dispatch_ride(evt->strategy);    /* vtable+0x60 */
    else
        state_op_dispatch_lock(evt->strategy, 0); /* vtable+0x58 */
}

/* state_operational_on_shutdown - OEM 0x131890.
 * "power off" bike-VM event: stop the idle-timeout timer (strategy+0xd0),
 * tell the power manager to shut down, and run the global teardown hook. */
void state_operational_on_shutdown(struct state_operational_evt *evt)
{
    state_operational_strategy *self = evt->strategy;

    ux_timer_stop(self->idle_timeout_timer);   /* strategy+0xd0 (FUN_00186fa0) */
    power_shutdown(ux_power(self->svc));        /* +0xb90 (FUN_0013dbb0) */
    ux_global_teardown();                       /* FUN_00138e50 */
}

/* state_operational_idle_timeout - OEM 0x1312d0.
 * 8000ms idle-timeout fired: re-arm the idle-animation timer (strategy+0x180)
 * and ask the bike-state manager to transition out of OPERATIONAL
 * (publishes state=2 / STANDBY on the bike-VM state channel). */
void state_operational_idle_timeout(struct state_operational_evt *evt)
{
    state_operational_strategy *self = evt->strategy;

    ux_timer_rearm(self->idle_anim_timer);     /* strategy+0x180 (FUN_00186760) */
    bike_request_standby(ux_bike(self->svc));  /* FUN_0013e2a0: publish state=2 */
}

/* state_operational_idle_animation_tick - OEM 0x1317d0.
 * 1000ms idle-animation tick. After >0x32 (50) ticks the idle animation is
 * given up: both timers (strategy+0xd0 idle-timeout, strategy+0x180 idle-anim)
 * are stopped. Otherwise, unless an updated animation theme is active
 * (animmgr+0x1020), play the IDLE LED-ring animation (id 0x545, palette 0xdf)
 * on ring 2; then re-arm the animation timer. */
void state_operational_idle_animation_tick(struct state_operational_evt *evt)
{
    state_operational_strategy *self = evt->strategy;
    ux_service *svc = self->svc;

    if (self->idle_tick_count > 0x32) {
        ux_timer_stop(self->idle_timeout_timer);  /* strategy+0xd0 */
        ux_timer_stop(self->idle_anim_timer);     /* strategy+0x180 */
        return;
    }

    if (anim_theme_overridden(ux_sound(svc))) {   /* animmgr+0x1020 (FUN_0013dc30) */
        common_logf("devices/main/ux/src/state_operational_strategy.cpp", 0x4a, LOG_WARN,
                    "Ignoring IDLE animation on led rings due to updated animation theme");
        ux_timer_rearm(self->idle_anim_timer);
        return;
    }

    /* FUN_00147cd0(animmgr, ring=2, id=0x545, palette=0xdf, 0, repeat=0, flag=1) */
    led_ring_play(ux_sound(svc), 2, 0x545, 0xdf, 0, 0, 1);
    ux_timer_rearm(self->idle_anim_timer);
}

/* state_operational_apply_animation_theme - OEM 0x130f30.
 * Bike-VM "animation theme" event (event class 1, sub 1). Reads the active
 * theme out of the sound/anim manager (FUN_0013e340 fills a tagged variant) and
 * applies it; the bulk of the OEM body is the std::vector<variant> copy/destroy
 * of the decoded theme (VENDOR ABI), modelled here as fetch+apply+release. */
void state_operational_apply_animation_theme(struct state_operational_evt *evt)
{
    ux_service *svc = evt->strategy->svc;
    anim_theme_t theme;

    theme._opaque = 0;
    anim_theme_get(ux_sound(svc), &theme);   /* FUN_0013e340 */
    anim_theme_apply(&theme);
    anim_theme_release(&theme);
}

/*
 * state_operational_strategy_ctor - OEM 0x131900.
 *
 * Strategy object is 0x440 bytes. Layout used here (verified offsets):
 *   +0x08  state id        (param_2)
 *   +0x30  ux_service*     (param_3)
 *   +0x38  on-enter MQTT/listener bookkeeping (vendor)
 *   +0xd0  idle-timeout ux_timer
 *   +0xc8  idle-tick gate counter
 *   +0xcc  saved prior ride latch
 *   +0xc8  ride latch enum   (param_1[0x19])
 *   +0x180 idle-animation ux_timer
 *
 * Behaviour:
 *  - install vtable + sub-object vptrs (DAT_001f9d00 .. DAT_001f9e28)
 *  - register the strategy as a bike-VM state observer (FUN_00148fa0)
 *  - create the idle-timeout (1000ms) and idle-anim (8000ms) timer objects
 *  - register three direct listeners (idle_timeout, idle_animation_tick,
 *    on_lock_request) on the bike-VM (FUN_00186da0)
 *  - request unlock on the elock, set alarm state=1, restore the light state
 *  - if entering from STANDBY/OPERATIONAL (state & ~4 == 2) and the elock is
 *    locked: latch ride state {1,3}, arm the 3000ms idle-timeout, and clear the
 *    ride-flag if set; else mark ride latch = 2. Otherwise carry over the prior
 *    latch (param_1[0x19]).
 *  - register ~14 bike-VM event handlers (FUN_00148dc0 / FUN_001482a0):
 *      (0,3)  publish-unplugged          (state_charging)
 *      (1,2)  LAB_00130860
 *      (1,1)  apply_animation_theme
 *      (3,0)  motor_assist_on
 *      (3,1)  motor_assist_off
 *      (2,0)  on_charge_stop             (state_charging)
 *      (2,1)  on_unlock
 *      (2,2)  LAB_00132f70
 *      (4,2)  LAB_00133120
 *      (4,1)  LAB_00132d90
 *      (4,0)  LAB_00132db0
 *      (0xff,0) on_shutdown
 *      plus on-enter publish-unplugged (FUN_001482a0) and LAB_00130680 (0,3)
 */
void state_operational_strategy_ctor(state_operational_strategy *self,
                                     int state_id, ux_service *svc)
{
    /* --- object header / sub-vptrs --- */
    self->state_id = state_id;
    so_install_vtables(self);                 /* DAT_001f9d00 .. 001f9e28 */
    self->svc = svc;

    /* register strategy as a bike-VM state observer (FUN_00148fa0) */
    so_observer_register(&self->state_observer, ux_bike_states(svc) /* FUN_0013db80 */);

    self->ride_latch = -1;                     /* param_1[0x17] = 0xff..ff */
    self->idle_active = 0;                     /* +0xc4 */
    self->ride_state = 0;                       /* param_1[0x19] */
    so_listener_slot_init(&self->lis_timeout); /* FUN_001869f0 +0x1a */
    so_listener_slot_init(&self->lis_tick);    /* FUN_001869f0 +0x30 */

    /* idle-timeout timer object, 1000ms (FUN_00186ab0) */
    self->idle_timeout_timer = ux_timer_new(svc);
    so_timer_init(self->idle_timeout_timer, self, 1000);
    /* idle-animation timer object, 8000ms */
    self->idle_anim_timer = ux_timer_new(svc);
    so_timer_init(self->idle_anim_timer, self, 8000);

    so_listener_slot_init(&self->lis_lock);    /* FUN_001869f0 +0x72 */
    so_pre_enter(self);                         /* FUN_00181460 */

    /* register strategy with the ride manager (FUN_0013db70 -> FUN_00160b80) */
    ride_observer_register(ux_ride(svc), &self->ride_obs);

    /* --- elock / ride re-evaluation on entry --- *
     * (state & 0xfffffffb) == 2  <=>  state is STANDBY(2) or CHARGING(? bit2). */
    if ((self->state_id & ~4u) == 2u) {
        if (!elock_is_unlocked(ux_lock(svc))) {
            self->ride_state = 0x300000001ULL;       /* latch {1,3} */
            so_arm_idle_timeout(&self->lis_timeout, 3000); /* FUN_001870c0 */
            if (ride_flag_pending(ux_ride_flag(svc))) {    /* FUN_00198780 */
                ride_flag_clear(ux_ride_flag(svc));         /* FUN_00198750 */
            }
        } else {
            self->ride_state = 2;
        }
    } else {
        if (self->ride_state != 3) {
            int prev = self->ride_state;
            self->ride_state = 3;
            self->prev_ride_state = prev;          /* +0xcc */
        }
    }

    /* register the three direct bike-VM listeners (FUN_00186da0) */
    so_listener_register(&self->lis_timeout, self, state_operational_idle_timeout);
    so_listener_register(&self->lis_tick,    self, state_operational_idle_animation_tick);
    so_listener_register(&self->lis_lock,    self, state_operational_on_lock_request);

    /* request unlock + alarm-on + restore lights */
    elock_request_unlock(ux_lock(svc));
    alarm_set_state(ux_alarm(svc) /* FUN_0013dbd0 */, 1);

    /* register strategy with the ble/sensor observer map (FUN_0013dbe0) */
    so_sensor_observer_register(ux_sensor(svc), &self->sensor_obs);

    light_restore(ux_light(svc));               /* FUN_0013db60 -> light_restore */

    /* fetch + drop the light-state snapshot (FUN_00167030 + vector teardown) */
    {
        light_state_t ls;
        ls._opaque = 0;
        light_state_get(ux_light_store(svc), &ls);  /* FUN_0013dba0 -> FUN_00167030 */
        light_state_release(&ls);
    }

    /* mark the sound/anim manager active (FUN_0013db80 -> FUN_00147b00, 1) */
    anim_set_active(ux_sound(svc), 1);

    /* --- bike-VM event handler registration (FUN_001482a0 / FUN_00148dc0) --- */
    so_handler_on_enter(&self->state_observer, self, state_charging_publish_unplugged);
    so_handler_register(&self->state_observer, 0, 3, self, so_handler_lab_130680);
    so_handler_register(&self->state_observer, 1, 2, self, so_handler_lab_130860);
    so_handler_register(&self->state_observer, 1, 1, self, state_operational_apply_animation_theme);
    so_handler_register(&self->state_observer, 3, 0, self, state_operational_motor_assist_on);
    so_handler_register(&self->state_observer, 3, 1, self, state_operational_motor_assist_off);
    so_handler_register(&self->state_observer, 2, 0, self, state_charging_on_charge_stop);
    so_handler_register(&self->state_observer, 2, 1, self, state_operational_on_unlock);
    so_handler_register(&self->state_observer, 2, 2, self, so_handler_lab_132f70);
    so_handler_register(&self->state_observer, 4, 2, self, so_handler_lab_133120);
    so_handler_register(&self->state_observer, 4, 1, self, so_handler_lab_132d90);
    so_handler_register(&self->state_observer, 4, 0, self, so_handler_lab_132db0);
    so_handler_register(&self->state_observer, 0xff, 0, self, state_operational_on_shutdown);
}
