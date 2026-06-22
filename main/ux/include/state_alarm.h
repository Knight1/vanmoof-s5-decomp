/*
 * state_alarm.h -- AlarmStrategy (UXService alarm/theft-response state).
 * Per-module declarations for state_alarm.c; included AFTER ux_common.h.
 *
 * OEM: program "ux", AArch64, image base 0x100000.
 * Source file (verified log strings): devices/main/ux/src/state_alarm_strategy.cpp
 * Strategy object: 0x5c0 bytes, vtable 0x1fa030.
 */
#ifndef UX_STATE_ALARM_H
#define UX_STATE_ALARM_H

#include "ux_common.h"

/* ---- AlarmStrategy object layout (offsets in machine words / bytes) -------
 * Reconstructed from the ctor (0x135e60). Only the fields the app logic
 * touches are modelled; vendor sub-objects (std::function holders, the
 * subscription handles) are opaque blobs. The OEM object embeds:
 *   +0x00  vtable (&DAT_001fa030)
 *   +0x08  state id (param_2, the ux_state this strategy implements)
 *   +0x10..+0x20  bound trigger/lock/button sub-vtables
 *   +0x28  alarm-2 escalate timeout  (30000 -> word[5])
 *   +0x30  alarm-3 / secondary timeout (10000 -> word[6])
 *   +0x38  ux_service ctx (word[7])
 *   +0x40  sound/anim subscription block (param_1+8)
 *   +0xc0  IMU / alarm-trigger subscription handle (param_1+0x18)
 *   +0x3b0 alarm-3 escalation timer  (param_1+0x3b0)
 *   +0x510 alarm-2 escalation timer  (param_1+0x510)
 *   +0x3b0 region also reused by button anim subs
 * We keep the layout abstract: the .c stores the ux_service ctx and the
 * two timers, and treats the subscription handles as opaque.
 */
typedef struct elock_alarm_handle elock_alarm_handle; /* alarm OD on the elock/lock mgr */
typedef struct ux_sound_mgr     ux_sound_mgr;         /* sound mute/play sub-mgr (db_60) */
typedef struct ux_soundplay_mgr ux_soundplay_mgr;     /* sound/play publisher (db_c0 thunk dbc0) */
typedef struct ux_anim_mgr      ux_anim_mgr;          /* LED-ring/anim mgr (db_80) */
typedef struct ux_ble_mgr       ux_ble_mgr;           /* BLE/button mgr (dbf0) */
typedef struct alarm_sub        alarm_sub;            /* opaque subscription handle */

typedef struct AlarmStrategy {
    void       *vtable;        /* +0x00 = &alarm_strategy_vtable (0x1fa030) */
    int         state_id;      /* +0x08 = which ux_state (UX_ALARM) */
    unsigned    timeout_a;     /* +0x28 = 30000 ms */
    unsigned    timeout_b;     /* +0x30 = 10000 ms */
    ux_service *ctx;           /* +0x38 = owning UXService */
    alarm_sub  *imu_sub;       /* +0xc0 = IMU/alarm-trigger subscription */
    ux_timer   *escalate_timer; /* +0x3b0 = alarm-3 escalation oneshot */
    ux_timer   *alarm2_timer;  /* +0x510 = alarm-2 -> alarm-3 escalation oneshot */
} AlarmStrategy;

/* ---- alarm-level OD accessor (FUN_00145cf0): byte at lock-mgr +0x80 ------- */
enum alarm_level {
    ALARM_LEVEL_THEFT255 = 0xFF, /* full theft */
    ALARM_LEVEL_3        = 0x03,
    ALARM_LEVEL_2        = 0x02
};
unsigned char elock_alarm_get_level(elock_alarm_handle *a); /* FUN_00145cf0 */
void          alarm_set_state(elock_alarm_handle *a, int level, int flag); /* alarm_set_state */

/* ---- sub-manager accessors (the FUN_0013db* / db-family ctx thunks) ------- *
 * Wrapped so state_alarm.c reads naturally; each maps 1:1 to an OEM thunk.   */
elock_alarm_handle *alarm_lock_od (ux_service *ctx);  /* FUN_0013dbd0 */
elock_alarm_handle *alarm_lock_od2(ux_service *ctx);  /* FUN_0013dbe0 */
ux_sound_mgr       *alarm_sound   (ux_service *ctx);  /* FUN_0013db60 */
ux_anim_mgr        *alarm_anim    (ux_service *ctx);  /* FUN_0013db80 */
ux_soundplay_mgr   *alarm_soundpub(ux_service *ctx);  /* FUN_0013dbc0 */
ux_ble_mgr         *alarm_ble     (ux_service *ctx);  /* FUN_0013dbf0 */
void               *alarm_mgr_90  (ux_service *ctx);  /* FUN_0013db90 */

/* sound/anim/ble primitives (verified at the call sites) */
void alarm_sound_mute     (ux_sound_mgr *s);                 /* FUN_00158a20 */
void alarm_sound_unmute   (ux_sound_mgr *s);                 /* FUN_0015d750 */
void alarm_findmy_stop    (ux_soundplay_mgr *p);            /* FUN_00198750 */
void alarm_anim_pattern   (ux_anim_mgr *a, int prio, int id,
                           int dur, int loop, int q);        /* FUN_00147cd0 */
void alarm_anim_set_mode  (ux_anim_mgr *a, int mode);        /* FUN_00147f00 */
void alarm_soundplay      (ux_soundplay_mgr *p, const char *name,
                           int a, int b, int c, int vol,
                           void *done_ctx);                  /* FUN_001996e0 */
bool alarm_ble_present     (ux_ble_mgr *b);                 /* FUN_00171480 */
bool alarm_ble_authorized  (ux_ble_mgr *b);                /* FUN_00171470 */
void alarm_ble_clear_alarm (ux_ble_mgr *b);                /* FUN_001725d0 */
bool alarm_sub_flag_get    (alarm_sub *s);                 /* FUN_00170230 */
void alarm_sub_flag_set    (alarm_sub *s, bool v);         /* FUN_00170250 */
int  alarm_button_mode     (void *mgr);                    /* FUN_00187c70 */
void alarm_return_to_state (ux_service *ctx);              /* FUN_0013e2a0 */
void alarm_timer_stop      (ux_timer *t);                  /* FUN_00186fa0 */

#endif /* UX_STATE_ALARM_H */
