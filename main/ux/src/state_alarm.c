/*
 * state_alarm.c -- VanMoof S5 i.MX8 `ux` service: AlarmStrategy.
 *
 * The theft-response / alarm state of the UXService state machine. Drives the
 * 3-stage theft escalation (Alarm 2 -> Alarm 3 -> Theft 255), the alarm sound
 * loop, the LED-ring flash, and the power-button / lock-state reactions.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000).
 * OEM source: devices/main/ux/src/state_alarm_strategy.cpp
 * Log strings, line numbers, sound names, animation ids and timeouts are
 * reproduced verbatim from the decompiled image.
 *
 * Vendor glue NOT reconstructed here (modelled at the call site only):
 *   - std::function<>/std::unordered_map subscription machinery
 *     (state_alarm_strategy_*_cb_manager, *_map_rehash_*, FUN_001702xx,
 *      FUN_00148*, FUN_00170260, FUN_001869f0, FUN_00186da0, FUN_00160b80)
 *   - the elock/sound/anim/ble sub-managers (opaque handles)
 */

#include "ux_common.h"
#include "state_alarm.h"

#define ALARM_SRC "devices/main/ux/src/state_alarm_strategy.cpp"

static const char *const ALARM_SOUND_URGENT = "alarm_2_urgent_loop"; /* DAT_001afea8, 19 bytes */

/* forward decls (intra-module dispatch) */
void state_alarm_strategy_trigger_for_level(AlarmStrategy *self, unsigned char arm);
void state_alarm_strategy_trigger_theft255 (AlarmStrategy *self);
void state_alarm_strategy_trigger_alarm2   (AlarmStrategy *self, char arm_escalation);
void state_alarm_strategy_trigger_alarm3   (AlarmStrategy *self);
void state_alarm_strategy_theft255_on_sound_done(AlarmStrategy **self_ref);
void state_alarm_strategy_on_lock_changed  (AlarmStrategy **self_ref);
void state_alarm_strategy_on_button        (AlarmStrategy **self_ref);
void state_alarm_strategy_alarm2_timeout_escalate(AlarmStrategy **self_ref);

/* The vendor callback bridges register a bound member-fn via these symbols.
 * They are STL std::function managers -- declared here as the OEM call site
 * sees them, never reconstructed. */
extern void state_alarm_strategy_imu_cb_manager(void *a, void *b, int op);
extern void state_alarm_strategy_lock_cb_manager(void *a, void *b, int op);
extern void state_alarm_strategy_button_cb_manager(void *a, void *b, int op);
extern void state_alarm_strategy_sounddone_cb_manager(void *a, void *b, int op);
extern void state_alarm_strategy_timer_cb_manager(void *a, void *b, int op);

/* =========================================================================
 * trigger_theft255 -- OEM 0x1354a0 (state_alarm_strategy.cpp:0x51)
 *
 * Theft level 255 (full theft). Logs the trigger, mutes the bike sound, then
 * publishes the urgent alarm tone (non-looping variant) to sound/play and
 * registers a sound-done callback -> theft255_on_sound_done.
 * =======================================================================*/
void state_alarm_strategy_trigger_theft255(AlarmStrategy *self)
{
    common_logf(ALARM_SRC, 0x51, LOG_WARN, "Alarm Theft 255 - Triggered", 0);

    /* mute the bike sound mixer */
    alarm_sound_mute(alarm_sound(self->ctx));

    /* publish the urgent tone, non-looping (loop=0), with a sound-done cb.
     * FUN_001996e0(pub, name, 1, 1, 0, 100, done_ctx). The cb-manager binds
     * state_alarm_strategy_theft255_on_sound_done(self). */
    alarm_soundplay(alarm_soundpub(self->ctx), ALARM_SOUND_URGENT,
                    1, 1, 0, 100, &self);
}

/* =========================================================================
 * trigger_alarm2 -- OEM 0x135630 (state_alarm_strategy.cpp:0x66)
 *
 * Alarm stage 2 (urgent). Flashes the LED ring (pattern 0x249, prio 3,
 * dur 0x58, loop), mutes the bike sound, plays the urgent loop tone
 * (looping). When arm_escalation is set, arms a 120000 ms one-shot timer
 * -> alarm2_timeout_escalate, which promotes to Alarm 3.
 * =======================================================================*/
void state_alarm_strategy_trigger_alarm2(AlarmStrategy *self, char arm_escalation)
{
    common_logf(ALARM_SRC, 0x66, LOG_WARN, "Alarm 2 - Triggered");

    /* LED-ring flash: FUN_00147cd0(anim, 3, 0x249, 0x58, 0, 1) */
    alarm_anim_pattern(alarm_anim(self->ctx), 3, 0x249, 0x58, 0, 1);

    /* mute bike sound, then play urgent loop tone (loop=1, no done cb) */
    alarm_sound_mute(alarm_sound(self->ctx));
    alarm_soundplay(alarm_soundpub(self->ctx), ALARM_SOUND_URGENT,
                    1, 0, 1, 100, NULL);

    if (arm_escalation != 0) {
        /* 120000 ms one-shot -> alarm2_timeout_escalate (-> Alarm 3) */
        ux_timer_arm_oneshot(self->alarm2_timer,
                             (ux_timer_cb)state_alarm_strategy_alarm2_timeout_escalate,
                             &self, 120000u);
    }
}

/* =========================================================================
 * trigger_alarm3 -- OEM 0x135d70 (state_alarm_strategy.cpp:0x5d)
 *
 * Alarm stage 3 (highest). Logs, stops the find-my / tracking publish, clears
 * the IMU subscription flag, mutes sound, sets the LED driver to mode 3, and
 * cancels the alarm-3 escalation timer.
 * =======================================================================*/
void state_alarm_strategy_trigger_alarm3(AlarmStrategy *self)
{
    common_logf(ALARM_SRC, 0x5d, LOG_WARN, "Alarm 3 - Triggered");

    alarm_findmy_stop(alarm_soundpub(self->ctx)); /* FUN_00198750 */
    alarm_sub_flag_set(self->imu_sub, false);     /* FUN_00170250(+0xc0, 0) */

    alarm_sound_mute(alarm_sound(self->ctx));     /* FUN_0015d750 path: db60+158a20 */
    alarm_anim_set_mode(alarm_anim(self->ctx), 3);/* FUN_00147f00(anim, 3) */

    alarm_timer_stop(self->escalate_timer);       /* FUN_00186fa0(+0x3b0) */
}

/* =========================================================================
 * trigger_for_level -- OEM 0x135de0
 *
 * Reads the current alarm level off the elock/alarm OD and dispatches the
 * matching stage: 0xFF -> Theft255, 0x03 -> Alarm3, 0x02 -> Alarm2(arm).
 * Any other level is ignored.
 * =======================================================================*/
void state_alarm_strategy_trigger_for_level(AlarmStrategy *self, unsigned char arm)
{
    unsigned char level = elock_alarm_get_level(alarm_lock_od(self->ctx));

    if (level == ALARM_LEVEL_3) {
        state_alarm_strategy_trigger_alarm3(self);
        return;
    }
    if (level == ALARM_LEVEL_THEFT255) {
        state_alarm_strategy_trigger_theft255(self);
        return;
    }
    if (level == ALARM_LEVEL_2) {
        state_alarm_strategy_trigger_alarm2(self, (char)arm);
    }
}

/* =========================================================================
 * alarm2_timeout_escalate -- OEM 0x1364f0
 *
 * The 120s Alarm-2 timer fired without reset: write alarm level 3 into the OD
 * (alarm_set_state(od, 3, 1)) and run the Alarm-3 trigger.
 * =======================================================================*/
void state_alarm_strategy_alarm2_timeout_escalate(AlarmStrategy **self_ref)
{
    AlarmStrategy *self = *self_ref;

    alarm_set_state(alarm_lock_od(self->ctx), 3, 1);
    state_alarm_strategy_trigger_alarm3(self);
}

/* =========================================================================
 * on_button -- OEM 0x1352f0  (power-button event handler)
 *
 * If a BLE peer is present and authorized and the IMU-sub flag is clear, the
 * button clears the BLE alarm. Otherwise, when the IMU-sub flag is clear, the
 * button flashes a short LED acknowledgement (pattern 0x2fb, prio 3, dur 0xc,
 * loop, queued).
 * =======================================================================*/
void state_alarm_strategy_on_button(AlarmStrategy **self_ref)
{
    AlarmStrategy *self = *self_ref;

    if (alarm_ble_present(alarm_ble(self->ctx))) {
        if (alarm_ble_authorized(alarm_ble(self->ctx)) &&
            !alarm_sub_flag_get(self->imu_sub)) {
            alarm_ble_clear_alarm(alarm_ble(self->ctx)); /* FUN_001725d0 */
            return;
        }
    }

    if (alarm_sub_flag_get(self->imu_sub))
        return;

    /* short button-ack LED flash: FUN_00147cd0(anim, 3, 0x2fb, 0xc, 1, 1) */
    alarm_anim_pattern(alarm_anim(self->ctx), 3, 0x2fb, 0xc, 1, 1);
}

/* =========================================================================
 * on_lock_changed -- OEM 0x135d00  (lock-state-changed handler)
 *
 * While the IMU-sub flag is clear and the alarm level is not 2, hand control
 * back to the UXService state evaluator (alarm_return_to_state). In every
 * other case just cancel the alarm-3 escalation timer.
 * =======================================================================*/
void state_alarm_strategy_on_lock_changed(AlarmStrategy **self_ref)
{
    AlarmStrategy *self = *self_ref;

    if (!alarm_sub_flag_get(self->imu_sub)) {
        if (elock_alarm_get_level(alarm_lock_od(self->ctx)) != ALARM_LEVEL_2) {
            alarm_return_to_state(self->ctx); /* FUN_0013e2a0 */
            return;
        }
    }
    alarm_timer_stop(self->escalate_timer); /* FUN_00186fa0(+0x3b0) */
}

/* =========================================================================
 * theft255_on_sound_done -- OEM 0x1353b0  (urgent-tone finished callback)
 *
 * After the theft tone completes: unmute the sound mixer, set the LED driver
 * to mode 3, and return control to the UXService state evaluator.
 * =======================================================================*/
void state_alarm_strategy_theft255_on_sound_done(AlarmStrategy **self_ref)
{
    AlarmStrategy *self = *self_ref;

    alarm_sound_unmute(alarm_sound(self->ctx));   /* db60 -> FUN_0015d750 */
    alarm_anim_set_mode(alarm_anim(self->ctx), 3);/* FUN_00147f00(anim, 3) */
    alarm_return_to_state(self->ctx);             /* FUN_0013e2a0 */
}

/* =========================================================================
 * state_alarm_strategy_ctor -- OEM 0x135e60
 *
 * AlarmStrategy constructor / on-enter. Sets the vtable + bound sub-vtables,
 * the escalation timeouts (30000 ms, 10000 ms) and the owning UXService ctx,
 * then wires up the three subscriptions the alarm state listens on:
 *   - IMU / alarm-trigger feed   -> trigger_for_level
 *   - lock-state-changed         -> on_lock_changed
 *   - power-button (code 0xff)   -> on_button
 * The IMU-sub flag is initialised from the button mode (2 == suppressed).
 * Finally it tail-calls trigger_for_level(self, 1) to enter the live level.
 *
 * The unordered_map insert / std::function bind / subscription bookkeeping
 * (FUN_0013db..-keyed maps, *_cb_manager, *_map_rehash_*) is vendor STL glue;
 * it is modelled below only as the binding calls it performs.
 * =======================================================================*/
void state_alarm_strategy_ctor(AlarmStrategy *self, int state_id, ux_service *ctx)
{
    self->vtable    = (void *)0; /* &alarm_strategy_vtable (0x1fa030) at OEM */
    self->state_id  = state_id;
    self->timeout_a = 30000;     /* word[5] */
    self->timeout_b = 10000;     /* word[6] */
    self->ctx       = ctx;
    self->imu_sub   = (alarm_sub *)0;
    self->escalate_timer = ux_timer_new(ctx);
    self->alarm2_timer   = ux_timer_new(ctx);

    /* IMU / alarm-trigger subscription -> trigger_for_level(self, 1)
     * (OEM binds via state_alarm_strategy_imu_cb_manager + LAB_00136450,
     *  registered into the sound/anim mgr at FUN_0013db80(ctx)). */
    mqtt_subscribe(ux_mqtt(ctx), "ux/alarm/trigger",
                   (mqtt_handler)(ux_fn)state_alarm_strategy_trigger_for_level, self);

    /* lock-state-changed subscription -> on_lock_changed
     * (OEM: FUN_0013db90(ctx) mgr + state_alarm_strategy_lock_cb_manager). */
    mqtt_subscribe(ux_mqtt(ctx), "ux/lock/changed",
                   (mqtt_handler)(ux_fn)state_alarm_strategy_on_lock_changed, self);

    /* seed the IMU-sub suppression flag from the current button mode
     * (FUN_00187c70 on FUN_0013dbe0(ctx)+0x70): mode 2 -> suppressed. */
    alarm_sub_flag_set(self->imu_sub, alarm_button_mode(alarm_lock_od2(ctx)) != 2);

    /* power-button (event code 0xff) subscription -> on_button
     * (OEM: FUN_00148dc0(anim, 0xff, 3, cb) + button_cb_manager). */
    mqtt_subscribe(ux_mqtt(ctx), "ux/button/0xff",
                   (mqtt_handler)(ux_fn)state_alarm_strategy_on_button, self);

    /* enter the currently-active alarm level, arming escalation */
    state_alarm_strategy_trigger_for_level(self, 1);
}
