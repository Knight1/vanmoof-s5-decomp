/* state_charging.h — StateChargingStrategy (UX charging state). OEM program
 * "ux" (AArch64, base 0x100000), TU
 * devices/main/ux/src/state_charging_strategy.cpp. Strategy object is 0xe8
 * bytes; only app-touched fields are named. */
#ifndef STATE_CHARGING_H
#define STATE_CHARGING_H

#include "ux_common.h"

/* StateChargingStrategy — created on entry to UX_CHARGING. */
typedef struct state_charging_strategy {
    void       *vtable;        /* +0x00  primary vtable                    */
    int         prev_state;    /* +0x08  previous ux_state (ctor param_2)   */
    ux_service *svc;           /* +0x20  UXService context (param_1[4])     */
    unsigned char led_kind;    /* +0x28  SOC LED-ring animation kind        */
    unsigned char animate_in;  /* +0xe0  one-shot "animate transition" flag */
    /* +0x30 the SOC-tick subscription block (db_e0 chained mgr)             */
    /* +0x30 (db_e0 path) is also used by the charge-stop / unplug handlers  */
} state_charging_strategy;

/* ---- module-local framework call-site externs (not in ux_common.h) ------- */
extern void sound_play_id(void *sound_mgr, int chan, int sound_id, int a3,
                          int a4, int a5);                 /* FUN_00147cd0 */
extern void sound_play_id6(void *sound_mgr, int chan, int sound_id, int a3,
                           int a4, int a5, int a6);        /* FUN_00147cd0 (7-arg) */
extern void cmd_publish_str(void *cmd_mgr, const char *value, int len); /* FUN_001996e0 */
extern void bike_play_startup_feedback(ux_service *svc, int a2, int direct);
extern void *ux_mqtt_cmd(ux_service *svc);   /* FUN_0013dbc0 (cmd publisher)  */
extern void *ux_bike2(ux_service *svc);      /* FUN_0013dbe0 (bike-state mgr) */
extern void *ux_lock_b(ux_service *svc);     /* FUN_0013db60                  */
extern bool standby_consume_shipping_flag(ux_service *svc); /* FUN_0013f2b0    */
extern unsigned FUN_theft_flags(void *bike_mgr);            /* FUN_0014fc80    */
/* render the SOC LED-ring animation (sound/anim mgr) — FUN_00147f20 */
extern void soc_render(void *sound_mgr, unsigned char led_kind,
                       unsigned char animate, int a3);
/* "animation theme overridden" query (FUN_0013dc30) */
extern char anim_theme_overridden(ux_service *svc);
/* charge-already-handled guard (FUN_0013dc50) */
extern char charge_stop_handled(void *charge_mgr);
/* charge UI start / teardown helpers (FUN_00158ad0 / FUN_00158b80) */
extern void charge_ui_start(void);
extern void charge_ui_teardown(void);
/* shutdown teardown helper shared with standby (FUN_0015d750) */
extern void standby_shutdown(void);
/* update-led-ring style accessor for charge start (FUN_00147b00) */
extern void led_ring_mode(void *sound_mgr, int mode);

state_charging_strategy *state_charging_strategy_ctor(state_charging_strategy *self,
                                                      int prev_state, ux_service *svc);
void state_charging_publish_plugged(state_charging_strategy *self);
void state_charging_soc_animation_tick(state_charging_strategy **slot);
void state_charging_on_charge_stop(state_charging_strategy **slot);
void state_charging_publish_unplugged(state_charging_strategy **slot,
                                      unsigned char *now_plugged,
                                      unsigned char *was_plugged);

#endif /* STATE_CHARGING_H */
