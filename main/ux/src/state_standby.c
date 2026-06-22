/*
 * state_standby.c — reconstructed VanMoof UX "standby" state strategy.
 *
 * OEM program "ux" (AArch64, image base 0x100000), translation unit
 * devices/main/ux/src/state_standby_strategy.cpp. Behaviour-oriented C11.
 *
 * The strategy is constructed every time the UXService enters UX_STANDBY. Its
 * ctor wires up event subscriptions (bike events, a 15000ms idle timer, an
 * 0xff/3 event handler), then dispatches on the previous state:
 *   - from shipping (0): consume the one-shot "shipping" flag; if set, direct
 *     power-on; else fall through to the standard power-button arm logic.
 *   - from alarm(6)/operational(3): run shutdown (sound id 0x783 if alarm
 *     reason==1 else 0x624), tear down, and (when coming from operational)
 *     publish "bike_shutdown".
 * Finally it always arms the power button.
 *
 * The std::function / std::unordered_map subscription plumbing (the
 * fn_manager / alloc_buckets / map_rehash VENDOR entries) is modelled at the
 * call site as plain subscribe_* calls; the bucket/rehash internals are not
 * reconstructed.
 */
#include "ux_common.h"
#include "state_standby.h"

extern unsigned FUN_theft_flags(void *bike_mgr);
/* standby shipping-cue sub-gate (FUN_0014fc70) + cue player (FUN_00147f00). */
extern char standby_subgate(void *bike_mgr);
extern void standby_play_cue(void *sound_mgr, int mode);

/* event subscription helpers (the std::function manager call sites). */
extern void standby_sub_bike_event(void *bike_mgr, void *self);          /* FUN_00170260 + db_80 chain */
extern void standby_sub_idle_timer(void *timer, void *self, unsigned ms);/* FUN_00186da0 */
extern void standby_sub_event_ff3 (void *bike_mgr, int a, int b, void *self); /* FUN_00148dc0 */

/* ---- state_standby_arm_power_button — OEM 0x133920 -----------------------
 * Arms the power button. In theft state (elock locked) stays PB_LOCKED and
 * just refreshes the "allowed" flag from the bike state; otherwise sets
 * PB_LONG_PRESS and arms a 3000ms long-press timer that requests power-on. */
void state_standby_arm_power_button(state_standby_strategy *self)
{
    void *lock = ux_lock_alt(self->svc);

    if (elock_is_unlocked(lock) == 0) {
        /* not theft: arm long-press path */
        pb_allow_set((char *)self + 0x150, 0);
        self->pb_state = PB_LONG_PRESS;
        /* 3000ms one-shot long-press -> state_standby_request_power_on */
        standby_sub_idle_timer((char *)self + 0x4f0, self, 3000);
    } else {
        /* theft: stay locked, allowed iff bike-state != kTheft */
        self->pb_state = PB_LOCKED;
        void *bike = ux_bike2(self->svc);
        int st = bike_state_get((char *)bike + 0x70);
        pb_allow_set((char *)self + 0x150, st != BIKE_STATE_THEFT);
    }
}

/* ---- state_standby_request_power_on — OEM 0x133a50 -----------------------
 * Requests a power-on transition unless we are already mid long-press and the
 * request did not come from the button. */
void state_standby_request_power_on(state_standby_strategy *self,
                                    char from_button, char direct)
{
    if (from_button != 0 || self->pb_state != PB_LONG_PRESS) {
        self->pb_state = PB_REQUESTED;
        pb_allow_set((char *)self + 0x150, 0);
        ux_request_power_on(self->svc, from_button, direct);
    }
}

/* ---- state_standby_on_power_button — OEM 0x133c20 ------------------------
 * Power-button press handler. If the bike is unlocked, play startup feedback;
 * otherwise request a (button-originated) power-on. */
void state_standby_on_power_button(state_standby_strategy *self, char direct)
{
    void *lock = ux_lock_alt(self->svc);

    if (elock_is_unlocked(lock) != 0) {
        bike_play_startup_feedback(self->svc, 1, direct);
        return;
    }
    state_standby_request_power_on(self, 1, direct);
}

/* ---- state_standby_direct_power_on — OEM 0x133c90 ------------------------
 * Direct (non-button) power-on. If unlocked and the kTheft latch (db+0xdb) is
 * set, play the charge/feedback sound (id 0x4b1) and bail. Otherwise, if the
 * bike-state is not kTheft, request a direct power-on; if it IS kTheft, log
 * the refusal (WARN, line 200). */
void state_standby_direct_power_on(state_standby_strategy *self)
{
    void *lock = ux_lock_alt(self->svc);

    if (elock_is_unlocked(lock) != 0) {
        void *lock2 = ux_lock_alt(self->svc);
        if (*((char *)lock2 + 0xdb) != 0) {        /* kTheft latch */
            void *snd = ux_sound(self->svc);
            sound_play_id(snd, 3, 0x4b1, 0x94, 1, 1);
            return;
        }
    }

    void *bike = ux_bike2(self->svc);
    int st = bike_state_get((char *)bike + 0x70);
    if (st != BIKE_STATE_THEFT) {
        ux_request_power_on(self->svc, 1, 0);
        return;
    }
    common_logf("devices/main/ux/src/state_standby_strategy.cpp", 200, LOG_INFO,
                "Bike is in kTheft state, direct power on not allowed");
}

/* ---- state_standby_strategy_ctor — OEM 0x133d50 -------------------------- */
state_standby_strategy *state_standby_strategy_ctor(state_standby_strategy *self,
                                                    int prev_state, ux_service *svc)
{
    self->vtable      = (void *)0; /* OEM stores &DAT_001f9e80 + sub-vtables */
    self->prev_state  = prev_state;
    self->svc         = svc;

    /* subscribe bike events + a generic event handler (std::function mgr). */
    standby_sub_bike_event(ux_sound(svc), self);

    /* register strategy with the lock/light/ride/bike event maps (the three
     * VENDOR unordered_map insert blocks: db_d0, db_c0, db_e0). */
    /* (map plumbing not reconstructed; effect is registering `self`.) */

    self->pb_state = 0;

    /* 15000ms idle timer (std::function manager call site). */
    standby_sub_idle_timer((char *)self + 0x88, self, 15000);

    if (prev_state == UX_SHIPPING - 1 /* 0 */) {
        if (standby_consume_shipping_flag(svc)) {
            state_standby_direct_power_on(self);
        } else {
            /* shipping flag not set (OEM 0x133d50): the theft-flag word gates
             * the branch. NOT theft -> if the sub-gate (FUN_0014fc70) is clear,
             * play the standby cue (FUN_00147f00(snd,3)). THEFT -> arm the normal
             * power-button path (state_standby_on_power_button). */
            void *bike = ux_bike2(svc);
            unsigned f = (unsigned)FUN_theft_flags(bike);   /* FUN_0014fc80 */
            if ((f & 0xff00) == 0) {
                if (standby_subgate(bike) == 0)             /* FUN_0014fc70 */
                    standby_play_cue(ux_sound(svc), 3);     /* FUN_00147f00(snd, 3) */
            } else {
                state_standby_on_power_button(self, 0);
            }
        }
    } else if (prev_state == UX_OPERATIONAL || prev_state == UX_ALARM) {
        /* play the shutdown sound: id 0x783 when alarm reason==1 else 0x624 */
        int *reason = ux_alarm_reason(svc);
        void *snd = ux_sound(svc);
        if (*reason == 1)
            sound_play_id(snd, 3, 0x783, 0x21, 1, 1);
        else
            sound_play_id(snd, 3, 0x624, 0x28, 1, 1);

        ux_lock_b(svc);
        standby_shutdown();

        if (prev_state == UX_OPERATIONAL) {
            void *cmd = ux_mqtt_cmd(svc);
            cmd_publish_str(cmd, "bike_shutdown", 0xd);
        }
    }

    /* register the 0xff/3 event handler, then arm the power button. */
    standby_sub_event_ff3(ux_sound(svc), 0xff, 3, self);
    state_standby_arm_power_button(self);

    return self;
}

/* theft-flag word from the bike-state manager (FUN_0014fc80). */
extern unsigned FUN_theft_flags(void *bike_mgr);
