/* state_standby.h — StateStandbyStrategy (UX standby state). OEM program "ux"
 * (AArch64, base 0x100000), TU devices/main/ux/src/state_standby_strategy.cpp.
 * Strategy object is 0x5a0 bytes; only the fields the reconstructed app logic
 * touches are modelled. Everything below the canonical ux_common.h surface is
 * declared here as module-local externs / a strategy struct. */
#ifndef STATE_STANDBY_H
#define STATE_STANDBY_H

#include "ux_common.h"

/* power-button state machine values stored at strategy+0x148 */
enum standby_pb_state {
    PB_LOCKED       = 1,  /* theft/locked: no power-on allowed            */
    PB_REQUESTED    = 2,  /* power-on has been requested                  */
    PB_LONG_PRESS   = 3   /* armed, waiting for the 3000ms long-press     */
};

/* bike-state manager state codes (ux_bike()+0x40 via bike_state_get) */
#define BIKE_STATE_THEFT 2   /* kTheft */

/* StateStandbyStrategy — object created on entry to UX_STANDBY.
 * The leading members mirror the OEM vtable/base layout; we only name the
 * fields the app logic reads. */
typedef struct state_standby_strategy {
    void       *vtable;          /* +0x00  primary vtable                   */
    int         prev_state;      /* +0x08  previous ux_state (ctor param_2)  */
    ux_service *svc;             /* +0x30  UXService context (param_1[6])    */
    int         pb_state;        /* +0x148 enum standby_pb_state             */
    /* +0x150 power-button "allowed" flag object (set via pb_allow_set)       */
    /* +0x4f0 the 3000ms long-press timer; +0x88 the 15000ms idle timer       */
} state_standby_strategy;

/* ---- module-local framework call-site externs (not in ux_common.h) ------- */
/* sound/animation manager: play sound id with params (OEM FUN_00147cd0) */
extern void sound_play_id(void *sound_mgr, int chan, int sound_id, int a3,
                          int a4, int a5);
/* request UX power-on; long_press=1 from button, allow operational (FUN_0013e1b0) */
extern void ux_request_power_on(ux_service *svc, int long_press, int direct);
/* power-button "allowed" flag setter at strategy+0x150 (FUN_00170250) */
extern void pb_allow_set(void *flag_obj, int allowed);
/* read bike-state code from bike-state mgr+0x70 (FUN_00187c70) */
extern int  bike_state_get(void *bike_mgr);
/* consume one-shot "shipping" init flag from storage (FUN_0013f2b0) */
extern bool standby_consume_shipping_flag(ux_service *svc);
/* bike startup feedback when already unlocked (bike_play_startup_feedback) */
extern void bike_play_startup_feedback(ux_service *svc, int a2, int direct);
/* lock/elock manager accessors distinct from the canonical ux_lock() */
extern void *ux_lock_alt(ux_service *svc);   /* FUN_0013db90 */
extern void *ux_lock_b(ux_service *svc);     /* FUN_0013db60 */
extern void *ux_bike2(ux_service *svc);      /* FUN_0013dbe0 (bike-state mgr) */
extern void *ux_mqtt_cmd(ux_service *svc);   /* FUN_0013dbc0 (cmd publisher) */
/* elock unlock query used as the theft gate (elock_is_unlocked) */
extern char elock_is_unlocked(void *lock_mgr);
/* shutdown teardown helper (FUN_0015d750) */
extern void standby_shutdown(void);
/* publish a string command via the cmd publisher (FUN_001996e0) */
extern void cmd_publish_str(void *cmd_mgr, const char *value, int len);
/* alarm/error-class reason accessor (FUN_0013dc10) -> *int */
extern int *ux_alarm_reason(ux_service *svc);

state_standby_strategy *state_standby_strategy_ctor(state_standby_strategy *self,
                                                    int prev_state, ux_service *svc);
void state_standby_arm_power_button(state_standby_strategy *self);
void state_standby_request_power_on(state_standby_strategy *self,
                                    char from_button, char direct);
void state_standby_on_power_button(state_standby_strategy *self, char direct);
void state_standby_direct_power_on(state_standby_strategy *self);

#endif /* STATE_STANDBY_H */
