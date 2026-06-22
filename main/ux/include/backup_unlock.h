/*
 * backup_unlock.h - module declarations for the reconstructed VanMoof S5 i.MX8
 * `ux` BackupUnlock handler (devices/main/ux/src/backup_unlock.cpp). Included
 * after ux_common.h.
 *
 * BackupUnlock implements the manual touch-keypad fallback unlock: the rider
 * taps a 3-symbol code on the bike; on a full plaintext match a success
 * callback fires, on a wrong code a deny tone plays and after three failures
 * the keypad locks out. NO cryptography is involved - the compare is a
 * byte-for-byte std::string match (matches the elock plaintext-unlock finding).
 */
#ifndef UX_BACKUP_UNLOCK_H
#define UX_BACKUP_UNLOCK_H

#include "ux_common.h"

#define BACKUP_CODE_LEN 3   /* stored code must be exactly 3 symbols */

/*
 * BackupUnlock object (OEM field offsets preserved as `self+0x..`). Vendor
 * sub-objects (the entered-digit std::string at +0x18..+0x20, the success
 * std::function at +0x2c8/+0x2d8/+0x2e0, the timers at +0xb8/+0x168/+0x218) are
 * kept opaque; only fields the app logic touches are named.
 */
typedef struct BackupUnlock {
    void   *vtable;            /* +0x00 */
    void   *mgr;               /* +0x08 UXService managers root (sound/light) */
    uint32_t conn_id;          /* +0x10 target connection/code id (sound key) */
    uint8_t  code_id_pad;      /* +0x14 */
    uint8_t _pad15[0x18 - 0x15];
    char   *entered_begin;     /* +0x18 entered-digit buffer begin */
    void   *entered_mid;       /* +0x20 entered-digit buffer end */
    uint8_t _pad28[0xB0 - 0x28];
    uint8_t  active;           /* +0xB0 backup-mode active flag */
    uint8_t  enabled;          /* +0xB1 backup unlock enabled */
    uint8_t  locked_out;       /* +0xB2 keypad locked out (3 fails) */
    uint8_t  fail_count;       /* +0xB3 consecutive wrong-code count */
    uint8_t _padB4[0xB8 - 0xB4];
    uint8_t  timeout_timer[0x168 - 0xB8]; /* +0xB8 entry/idle timeout timer */
    uint8_t  entry_timer[0x218 - 0x168];  /* +0x168 5s entry timeout timer */
    uint8_t  lockout_timer[0x2C8 - 0x218];/* +0x218 lockout-action timer */
    uint8_t  result_cb[0x2E8 - 0x2C8];    /* +0x2c8 success/fail std::function */
} BackupUnlock;

/* --- vendor framework helpers modelled at the call site -------------------- */

/* sound/light effect driver (FUN_00147cd0): play effect `id` for `dur` on the
 * given sound manager, keyed by connection id. The trailing flags are the
 * OEM's (loop/priority/queue) booleans, reproduced verbatim per call. */
extern void bu_effect(void *sound_mgr, uint32_t conn_id, int effect_id, int dur,
                      int f0, int f1, int f2);

/* sound manager accessor (FUN_0013db80) and light/feedback accessor
 * (FUN_0013dbc0). */
extern void *bu_sound_mgr(void *mgr);
extern void *bu_feedback_mgr(void *mgr);

/* light pattern routine (FUN_0013efc0): start LED routine `id`. */
extern void bu_light_routine(void *mgr, int id);

/* stored backup code (FUN_0013fc10): returns the code as a (begin,end) pair;
 * length 0 means "not set". Modelled as a heap C string + length. */
typedef struct bu_code { char *data; size_t len; } bu_code;
extern void bu_load_code(bu_code *out, void *mgr);
extern void bu_free_code(bu_code *c);

/* feedback string publish (FUN_001996e0): publish a short tagged feedback
 * token ("p_unlock" / "negative") to the keypad/LED feedback channel. */
extern void bu_publish_feedback(void *feedback_mgr, const char *token,
                                size_t len);

/* timers (FUN_00186da0 arm / FUN_00186fa0 commit / FUN_00186760 cancel). */
extern void bu_timer_arm   (void *timer, ux_timer_cb cb, void *ctx, long ms);
extern void bu_timer_commit(void *timer);
extern void bu_timer_cancel(void *timer);

/* success/fail result callback (self+0x2c8 std::function, invoked via +0x2e0).*/
extern void bu_fire_result(BackupUnlock *self, bool success);

/* OEM timer entry points referenced by start/onWrongCode. */
extern void bu_on_timeout(void *ctx);   /* FUN_0016fe50 (5s entry timeout) */
extern void bu_on_lockout(void *ctx);   /* FUN_0016ff40 (lockout action) */

void backup_unlock_start(BackupUnlock *self, uint32_t conn_id);
void backup_unlock_check_code(BackupUnlock *self);
void backup_unlock_on_wrong_code(BackupUnlock *self);
void backup_unlock_play_entry_feedback(BackupUnlock *self);

#endif /* UX_BACKUP_UNLOCK_H */
