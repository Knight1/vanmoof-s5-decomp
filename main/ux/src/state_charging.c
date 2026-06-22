/*
 * state_charging.c — reconstructed VanMoof UX "charging" state strategy.
 *
 * OEM program "ux" (AArch64, image base 0x100000), translation unit
 * devices/main/ux/src/state_charging_strategy.cpp. Behaviour-oriented C11.
 *
 * Constructed when the UXService enters UX_CHARGING. The ctor:
 *   - if entered from shipping(0): consume the shipping flag and either play
 *     startup feedback, or (if a theft check fails) publish a
 *     firmware_update_success / firmware_update_failed result and bail; else
 *     publishes "charger_plugged".
 *   - if entered from operational(3): tears down + publishes plugged.
 *   - always: arms the 3000ms SOC-animation tick, subscribes to charge events
 *     (via the db_e0 / db_50 manager maps), and selects the charge LED mode.
 *
 * The std::function / unordered_map plumbing (soc_fn_manager, alloc_buckets,
 * map_rehash_{a,b}) is VENDOR and modelled at the call site only.
 */
#include "ux_common.h"
#include "state_charging.h"

/* SOC-tick subscription (std::function manager call site, FUN_00186ea0). */
extern void charging_sub_soc_tick(void *mgr, void *self, unsigned ms);
/* register the charge-stop / unplug handlers in the event maps. */
extern void charging_sub_charge_events(void *bike_mgr, void *self);

/* ---- state_charging_publish_plugged — OEM 0x12edf0 -----------------------
 * Publishes "charger_plugged" to the command publisher, plays the charge-start
 * sound (id 0x696), and starts the charge UI. */
void state_charging_publish_plugged(state_charging_strategy *self)
{
    void *cmd = ux_mqtt_cmd(self->svc);
    cmd_publish_str(cmd, "charger_plugged", 0xf);

    void *snd = ux_sound(self->svc);
    sound_play_id6(snd, 3, 0x696, 0x31, 1, 0, 1);

    ux_lock_b(self->svc);
    charge_ui_start();
}

/* ---- state_charging_soc_animation_tick — OEM 0x12ed00 --------------------
 * 3000ms timer tick: render the battery-SOC LED-ring animation, unless an
 * updated animation theme overrides it (DEBUG, line 0x2d). */
void state_charging_soc_animation_tick(state_charging_strategy **slot)
{
    state_charging_strategy *self = *slot;

    if (anim_theme_overridden(self->svc) == 0) {
        void *snd = ux_sound(self->svc);
        soc_render(snd, self->led_kind, self->animate_in, 1);
        self->animate_in = 0;
        return;
    }
    common_logf("devices/main/ux/src/state_charging_strategy.cpp", 0x2d, LOG_WARN,
                "Ignoring SOC animation on led rings due to updated animation theme");
}

/* ---- state_charging_on_charge_stop — OEM 0x130400 ------------------------
 * Charge-stop event handler: if not already handled, tear down the charge UI. */
void state_charging_on_charge_stop(state_charging_strategy **slot)
{
    state_charging_strategy *self = *slot;
    void *lock = ux_lock_b(self->svc);   /* db_60 -> charge mgr */

    if (charge_stop_handled(lock) != 0)
        return;
    ux_lock_b(self->svc);
    charge_ui_teardown();
}

/* ---- state_charging_publish_unplugged — OEM 0x130b00 ---------------------
 * Charger-state change handler. When newly unplugged (was plugged, now not),
 * publishes "phone_plugged". The byte test ((*was ^ 1) & *now) is the OEM
 * edge detector. */
void state_charging_publish_unplugged(state_charging_strategy **slot,
                                      unsigned char *now_plugged,
                                      unsigned char *was_plugged)
{
    state_charging_strategy *self = *slot;

    if (((*was_plugged ^ 1) & *now_plugged) != 0) {
        void *cmd = ux_mqtt_cmd(self->svc);
        cmd_publish_str(cmd, "phone_plugged", 0xd);
    }
}

/* ---- state_charging_strategy_ctor — OEM 0x12ef30 ------------------------- */
state_charging_strategy *state_charging_strategy_ctor(state_charging_strategy *self,
                                                      int prev_state, ux_service *svc)
{
    self->vtable     = (void *)0; /* OEM stores &DAT_001f9c60 + sub-vtables */
    self->prev_state = prev_state;
    self->svc        = svc;
    self->led_kind   = 0xff;
    self->animate_in = 1;

    if (prev_state == 0 /* from shipping */) {
        if (standby_consume_shipping_flag(svc)) {
            bike_play_startup_feedback(svc, 1, 0);
            goto arm;
        }
        void *bike = ux_bike2(svc);
        unsigned f = FUN_theft_flags(bike);
        if ((f & 0xff00) != 0) {
            void *bike2 = ux_bike2(svc);
            unsigned f2 = FUN_theft_flags(bike2);
            if ((f2 & 0xff00) == 0) {
                /* OEM throws here; modelled as no-op */
                goto arm;
            }
            if ((f2 & 0xff) == 0) {
                /* update succeeded */
                void *snd = ux_sound(svc);
                sound_play_id(snd, 3, 0x4b1, 0x94, 1, 1);
                void *cmd = ux_mqtt_cmd(svc);
                cmd_publish_str(cmd, "firmware_update_success", 0x17);
            } else {
                /* update failed */
                void *snd = ux_sound(svc);
                sound_play_id(snd, 3, 0x3ce, 0x2f, 1, 1);
                void *cmd = ux_mqtt_cmd(svc);
                cmd_publish_str(cmd, "firmware_update_failed", 0x16);
            }
            goto arm;
        }
    } else if (prev_state == UX_OPERATIONAL) {
        ux_lock_b(svc);
        standby_shutdown();
    }

    state_charging_publish_plugged(self);

arm:
    /* arm the 3000ms SOC-animation tick (callback = soc_animation_tick). */
    charging_sub_soc_tick((char *)self + 0x30, self, 3000);
    /* subscribe charge-stop / unplug handlers into the event maps. */
    charging_sub_charge_events(ux_bike2(svc), self);

    void *snd = ux_sound(svc);
    led_ring_mode(snd, 1);

    return self;
}
