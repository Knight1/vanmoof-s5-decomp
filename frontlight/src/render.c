/*
 * render.c — front LED frame/animation renderer + frame-table store.
 *
 * VanMoof S5/A5 front LED light controller. Translated from the OEM image
 * (NXP LPC546xx Cortex-M4F, image base 0x0). The frame-timer wait and the OD
 * begin/end-frame markers are vendor; the gamma expansion, master-brightness
 * scaling and frame-table layout are VanMoof.
 */

#include "frontlight.h"

/* frontlight_render_frame — render one LED animation window.
 *
 * OEM disassembly (0x000028f8..0x00002a30):
 *
 * Zeroes a 16-byte per-LED channel scratch, arms the frame timer
 * (rtos_sem_take(*FL_PWM_CTX_PP, 0)), and range-checks the window: rejects if
 * start >= 300 or (start + count - 1) wraps past 300, returning 0. Sends the OD
 * begin-frame marker, then for each frame index in [0, count):
 *   - idx = (start + i) & 0xffff; if idx >= 300 it wraps (idx -= 300) and uses
 *     the flash wrap table, else the primary RAM table; entry = base + idx*0xc.
 *   - expands the 8 packed bytes at entry+2..+9 into 16 channels via the gamma
 *     LUT (high nibble then low nibble).
 *   - hold = entry[0] (u16); step_flag = entry[0xa] (u8).
 *   - if master brightness (FL_MASTER_BRIGHTNESS) != 0xff, scales each non-zero
 *     channel: v = (s16)c * (s16)master / 0xff, clamped to >= 1.
 *   - pushes all 16 channels via frontlight_led_set_brightness.
 *   - if step_flag <= 100 and anim->flags bit2 is clear, sets the PWM duty to
 *     step_flag (the per-frame duty percentage).
 *   - waits the hold time (rtos_sem_take, vestigial *1000/1000 ms scale); a
 *     return of 1 aborts: send end-frame marker, return 0.
 * On normal completion sends the end-frame marker and returns 1.
 */
int frontlight_render_frame(const frontlight_anim_t *anim)
{
    unsigned char ch_buf[16];
    unsigned char step_flag;
    void        **pwm_ctx_pp = (void **)FL_PWM_CTX_PP;
    const unsigned char *gamma;
    int tbl_primary;
    int tbl_wrap;
    int i;

    ch_buf[0] = 0;
    ch_buf[1] = 0;
    ch_buf[2] = 0;
    ch_buf[3] = 0;
    mem_set(&ch_buf[4], 0, 0xd);

    rtos_sem_take(*pwm_ctx_pp, 0);

    if (anim->start >= 0x296)
        return 0;
    if ((unsigned short)((unsigned short)(anim->start + anim->count) - 1) >= 0x296)
        return 0;

    od_frame_marker(anim->start);          /* begin-frame (marker carries start index) */

    gamma       = (const unsigned char *)FL_GAMMA_LUT;
    tbl_primary = (int)FL_FRAME_TABLE;
    tbl_wrap    = (int)FL_FRAME_TABLE_WRAP;

    for (i = 0; i < (int)(unsigned)anim->count; i++) {
        unsigned int idx  = (unsigned int)((unsigned)anim->start + i) & 0xffff;
        int          base = tbl_primary;
        const unsigned short *entry;
        unsigned char  master;
        unsigned short hold;
        int           k;
        unsigned char ch;

        if (idx >= 300) {
            idx  = (idx - 300) & 0xffff;
            base = tbl_wrap;
        }
        entry = (const unsigned short *)(idx * 0xc + base);

        /* expand 8 packed bytes (entry+2..+9) into 16 gamma-corrected channels */
        {
            int k2 = 0;
            unsigned char *dst = ch_buf;
            do {
                unsigned char b = *((const unsigned char *)entry + (k2 >> 1) + 2);
                k2 += 2;
                dst[0] = gamma[b >> 4];
                dst[1] = gamma[b & 0xf];
                dst += 2;
            } while (k2 != 0x10);
        }

        hold      = entry[0];
        step_flag = ((const unsigned char *)entry)[0xa];

        master = *(volatile unsigned char *)FL_MASTER_BRIGHTNESS;
        if (master != 0xff) {
            for (k = 0; k < 16; k++) {
                if (ch_buf[k] != 0) {
                    unsigned int v =
                        (unsigned int)((int)(short)ch_buf[k] * (int)(short)master) / 0xff;
                    ch_buf[k] = (v == 0) ? 1 : (unsigned char)v;
                }
            }
        }

        for (ch = 0; ch != 0x10; ch++)
            frontlight_led_set_brightness((unsigned char)ch, ch_buf[ch]);

        if (step_flag <= 0x64 && (anim->flags & 0x04) == 0)
            frontlight_pwm_set_duty(step_flag);

        if (rtos_sem_take(*pwm_ctx_pp, ((uint32_t)hold * 1000u) / 1000u) == 1) {
            od_frame_marker(0xffff);       /* aborted */
            return 0;
        }
    }

    od_frame_marker(0xffff);               /* end-frame */
    return 1;
}

/* frontlight_frame_table_store — store one LED frame into the frame table.
 *
 * OEM disassembly (0x000031a4..0x000031de):
 *
 * The record pointer arrives in the THIRD argument register (r2); the two
 * leading params are unused. rec[0] (u16) is the slot index. If index >= 300 it
 * logs the overflow event (src_tag FL_LOG_SRC_OVERFLOW, code 0x9e) and returns
 * -1. Otherwise slot = FL_FRAME_TABLE + index*0xc; writes the hold/colour
 * halfword (rec bytes 2|3<<8) at slot+0 and copies the 9 remaining bytes (rec+4)
 * to slot+2.
 */
int frontlight_frame_table_store(int unused1, int unused2, const unsigned short *rec)
{
    (void)unused1;
    (void)unused2;

    int base = (int)FL_FRAME_TABLE;

    if (rec[0] < 300) {
        int off = (unsigned)rec[0] * 0xc;
        const unsigned char *rb = (const unsigned char *)rec;
        *(volatile unsigned short *)(base + off) =
            (unsigned short)(rb[2] | (rb[3] << 8));
        mem_copy((void *)(base + off + 2), rec + 2, 9);
        return 0;
    }

    frontlight_log_event(FL_LOG_SRC_OVERFLOW, 0x9e, 0);
    return -1;
}
