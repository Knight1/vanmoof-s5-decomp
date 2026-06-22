/*
 * backup_unlock.c - reconstructed VanMoof S5 i.MX8 `ux` service BackupUnlock.
 * OEM source: devices/main/ux/src/backup_unlock.cpp. Program "ux" (AArch64,
 * base 0x100000). The manual touch-keypad fallback unlock. Vendor framework
 * (sound/light driver, feedback publisher, timers, the std::string code
 * buffer, the result std::function) is modelled via backup_unlock.h externs.
 * OEM addresses are quoted per function.
 */
#include "ux_common.h"
#include "backup_unlock.h"

#define BACKUP_CPP "devices/main/ux/src/backup_unlock.cpp"

/* Feedback tokens (DAT_001c58e0/s_p_unlock_001c58e5, _DAT_001b0358/s_negative,
 * both emitted with length 0xd). */
#define BU_TOKEN_ENTER "p_unlock"
#define BU_TOKEN_DENY  "negative"
#define BU_TOKEN_LEN   0xd

/* sound effect ids used by the keypad feedback (verbatim from FUN_00147cd0). */
#define BU_SND_DENY     0x2fb
#define BU_SND_DENY_DUR 0xc
#define BU_SND_ENTER_A      0x307
#define BU_SND_ENTER_A_DUR  0x14
#define BU_SND_ENTER_B      0x2a1
#define BU_SND_ENTER_B_DUR  0x1e

/* BackupUnlock entry feedback - OEM 0x16ffa0.
 * Two light/sound effects keyed on the connection id; called at the tail of
 * backup_unlock_start. */
void backup_unlock_play_entry_feedback(BackupUnlock *self)
{
    bu_effect(bu_sound_mgr(self->mgr), self->conn_id,
              BU_SND_ENTER_A, BU_SND_ENTER_A_DUR, 1, 0, 1);
    bu_effect(bu_sound_mgr(self->mgr), self->conn_id,
              BU_SND_ENTER_B, BU_SND_ENTER_B_DUR, 0, 0, 0);
}

/* BackupUnlock::start(code) - OEM 0x170808.
 * Enters manual-code unlock mode (only if idle and enabled). If no backup code
 * is stored, plays the deny tone and bails; otherwise starts the LED routine,
 * arms the 5s entry timeout, publishes the "p_unlock" feedback token, and plays
 * the entry feedback. */
void backup_unlock_start(BackupUnlock *self, uint32_t conn_id)
{
    bu_code code;

    /* Guard: not already active (+0xb0==0), enabled (+0xb1!=0),
     * not locked out (+0xb2==0). */
    if (self->active != 0 || self->enabled == 0 || self->locked_out != 0)
        return;

    bu_load_code(&code, self->mgr);
    {
        bool empty = (code.len == 0);
        bu_free_code(&code);
        if (empty) {
            common_logf(BACKUP_CPP, 0x35, LOG_WARN, "Backup unlock code not set");
            /* OEM deny tone FUN_00147cd0(mgr, key=3, effect=0x2fb, 0xc, 1, 1):
             * the sound-key arg is the literal 3, not the effect id. */
            bu_effect(bu_sound_mgr(self->mgr), 3, BU_SND_DENY,
                      BU_SND_DENY_DUR, 1, 1, 0);
            return;
        }
    }

    /* Code exists: enter backup mode. */
    bu_light_routine(self->mgr, 10);
    self->active = 1;
    self->enabled = 0;         /* OEM +0xb1 zero-write (one-shot entry) */
    self->fail_count = 0;      /* OEM clears the +0xb0..+0xb3 word's low bytes */
    self->locked_out = 0;
    self->conn_id = conn_id;
    self->code_id_pad = 0;

    /* Clear the entered-digit buffer (self+0x18 .. self+0x20). */
    {
        char *p = self->entered_begin;
        char *end = (char *)self->entered_mid;
        for (; end != p; ++p)
            *p = 0;
    }

    /* Publish the "p_unlock" feedback token to the keypad/LED channel. */
    bu_publish_feedback(bu_feedback_mgr(self->mgr), BU_TOKEN_ENTER, BU_TOKEN_LEN);

    /* Arm the 5s entry-timeout oneshot -> bu_on_timeout (FUN_0016fe50). */
    bu_timer_arm(self->entry_timer, bu_on_timeout, self, 5000);
    bu_timer_commit(self->entry_timer);

    backup_unlock_play_entry_feedback(self);
}

/* BackupUnlock::checkCode() - OEM 0x170e00.
 * Validates the entered code against the stored one. Cancels both timers,
 * requires an exactly-3-symbol stored code, and does a plaintext byte-by-byte
 * compare (NO crypto). On success fires the result callback with `true`; on
 * any mismatch / wrong length tail-calls onWrongCode. */
void backup_unlock_check_code(BackupUnlock *self)
{
    bu_code code;
    bool match = false;

    bu_timer_cancel(self->timeout_timer);
    bu_timer_cancel(self->entry_timer);

    bu_load_code(&code, self->mgr);

    if (code.len == BACKUP_CODE_LEN) {
        size_t entered_len =
            (size_t)((char *)self->entered_mid - self->entered_begin) & 0xff;
        size_t i = 0;

        match = true;
        while ((i & 0xff) < entered_len) {
            char c = self->entered_begin[i];
            if (c == 0 || code.data[i] != c) {
                match = false;
                break;
            }
            ++i;
        }
    }

    if (match && code.len == BACKUP_CODE_LEN) {
        common_logf(BACKUP_CPP, 0x9c, LOG_WARN, "Backup unlock success");
        bu_fire_result(self, true);
        self->active = 0;
    } else {
        backup_unlock_on_wrong_code(self);
    }

    bu_free_code(&code);
}

/* BackupUnlock::onWrongCode() - OEM 0x170be0.
 * Clears the active flag, publishes the "negative" feedback token, plays the
 * deny tone, fires the result callback with `false`, and bumps the failure
 * counter. After the third consecutive failure, latches the lockout flag,
 * schedules the lockout action, and resets the counter. */
void backup_unlock_on_wrong_code(BackupUnlock *self)
{
    uint8_t fails;

    self->active = 0;

    bu_publish_feedback(bu_feedback_mgr(self->mgr), BU_TOKEN_DENY, BU_TOKEN_LEN);
    bu_effect(bu_sound_mgr(self->mgr), self->conn_id, BU_SND_DENY,
              BU_SND_DENY_DUR, 1, 1, 0);

    bu_fire_result(self, false);

    fails = (uint8_t)(self->fail_count + 1);
    self->fail_count = fails;
    if (fails > 2) {
        self->locked_out = 1;
        /* Schedule the lockout action -> bu_on_lockout (FUN_0016ff40). */
        bu_timer_arm(self->lockout_timer, bu_on_lockout, self, 0);
        bu_timer_commit(self->lockout_timer);
        self->fail_count = 0;
    }
}
