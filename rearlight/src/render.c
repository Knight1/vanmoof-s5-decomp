/*
 * render.c — rear LED frame/animation renderer + frame-table store.
 *
 * VanMoof S5/A5 rear LED light controller. Translated from the OEM image
 * (NXP LPC546xx Cortex-M4F, image base 0x0). The frame-timer wait and the OD
 * begin/end-frame markers are vendor; the gamma expansion, channel remap,
 * master-brightness scaling and frame-table layout are VanMoof. Unlike the
 * frontlight, the rearlight drives 20 LED channels and has no pwm_set_duty.
 */

#include "rearlight.h"

/* rearlight_render_frame — render one LED animation window.
 *
 * OEM disassembly (0x00002798..0x0000296c):
 *
 * Zeroes a 20-byte per-LED channel scratch, drains the frame timer
 * (rtos_sem_take(*RL_FRAME_QUEUE, 0)), and range-checks the window: rejects if
 * start > 0x2a2 or (start + count - 1) wraps past 0x2a2, returning 0. Sends the
 * OD begin-frame marker (carrying start), then for each frame index:
 *   - phys = (start + i) & 0xffff; if phys >= 300 it wraps (phys -= 300) and
 *     uses the flash wrap table, else the primary RAM table; entry = base+phys*12.
 *   - expands the 10 packed bytes at entry+2 into 20 channels via the gamma LUT.
 *   - duration = entry[0] (u16).
 *   - if the remap-enable flag (RL_REMAP_ENABLE) is set, applies a fixed
 *     20-channel spatial permutation.
 *   - if master brightness (RL_MASTER_BRIGHTNESS) != 0xff, scales each non-zero
 *     channel: v = (s16)c * (s16)master / 0xff, clamped to >= 1.
 *   - pushes all 20 channels via rearlight_led_set_brightness.
 *   - waits the duration (vestigial *1000/1000 ms scale); a return of 1 aborts:
 *     send end-frame marker, return 0.
 * On normal completion sends the end-frame marker and returns 1.
 */
int rearlight_render_frame(const rl_anim_request_t *req)
{
    uint32_t      *frame_queue = (uint32_t *)RL_FRAME_QUEUE;
    const uint8_t *gamma       = (const uint8_t *)RL_GAMMA_LUT;
    const uint8_t *remap_en    = (const uint8_t *)RL_REMAP_ENABLE;
    const uint8_t *master_ptr  = (const uint8_t *)RL_MASTER_BRIGHTNESS;
    uint8_t       *table_a     = (uint8_t *)RL_FRAME_TABLE;
    uint8_t       *table_b     = (uint8_t *)RL_FRAME_TABLE_WRAP;

    uint8_t  chan[20];
    uint16_t start = req->start;
    int i;

    chan[0] = chan[1] = chan[2] = chan[3] = 0;
    mem_set(&chan[4], 0, 0x11);

    rtos_sem_take(*frame_queue, 0);

    if (start > 0x2a2)
        return 0;
    if ((uint16_t)((uint16_t)(req->count + start) - 1) > 0x2a2)
        return 0;

    od_frame_marker(start);                 /* begin-frame (carries start) */

    if ((int)(uint32_t)req->count <= 0) {
        od_frame_marker(0xffff);
        return 1;
    }

    for (i = 0; i < (int)(uint32_t)req->count; i++) {
        uint16_t phys = (uint16_t)(start + i);
        const uint8_t *base = table_a;
        const uint8_t *entry;
        const uint8_t *packed;
        uint16_t duration;
        uint8_t  master;
        int k;

        if (phys >= 300) {
            phys = (uint16_t)(phys - 300);
            base = table_b;
        }
        entry  = base + (uint32_t)phys * 12u;
        packed = entry + 2;

        /* gamma-expand 10 packed bytes -> 20 channel bytes */
        for (k = 0; k < 10; k++) {
            uint8_t b = packed[k];
            chan[2 * k]     = gamma[b >> 4];
            chan[2 * k + 1] = gamma[b & 0x0f];
        }

        duration = *(const uint16_t *)entry;

        if (*remap_en != 0) {
            uint8_t s[20];
            mem_copy(s, chan, 20);
            /* OEM channel-remap permutation: chan[j] = s[perm[j]] */
            chan[0]  = s[19]; chan[1]  = s[14]; chan[2]  = s[15]; chan[3]  = s[16];
            chan[4]  = s[17]; chan[5]  = s[18]; chan[6]  = s[1];  chan[7]  = s[2];
            chan[8]  = s[3];  chan[9]  = s[4];  chan[10] = s[5];  chan[11] = s[0];
            chan[12] = s[6];  chan[13] = s[7];  chan[14] = s[8];  chan[15] = s[9];
            chan[16] = s[10]; chan[17] = s[11]; chan[18] = s[12]; chan[19] = s[13];
        }

        master = *master_ptr;
        if (master != 0xff) {
            for (k = 0; k < 20; k++) {
                if (chan[k] != 0) {
                    uint32_t v = (uint32_t)((int16_t)chan[k] * (int16_t)master) / 0xff;
                    chan[k] = (v == 0) ? 1 : (uint8_t)v;
                }
            }
        }

        for (k = 0; k < 20; k++)
            rearlight_led_set_brightness((uint8_t)k, chan[k]);

        if (rtos_sem_take(*frame_queue, ((uint32_t)duration * 1000u) / 1000u) == 1) {
            od_frame_marker(0xffff);        /* aborted */
            return 0;
        }
    }

    od_frame_marker(0xffff);               /* end-frame */
    return 1;
}

/* rearlight_frame_table_store — store one LED frame into the frame table.
 *
 * OEM disassembly (0x000030e8..0x00003122):
 *
 * The record pointer arrives in the 3rd argument register (r2); the two leading
 * params are unused. rec[0] (u16) is the slot index. If index >= 300 it logs the
 * overflow event (src_tag RL_LOG_SRC_OVERFLOW, code 0xb2) and returns -1.
 * Otherwise slot = RL_FRAME_TABLE + index*0xc; writes the colour/hold halfword
 * (rec[2]|rec[3]<<8) at slot+0 and copies the remaining 0xb bytes (rec+4) to
 * slot+2.
 */
int rearlight_frame_table_store(uint32_t unused1, uint32_t unused2, const uint8_t *rec)
{
    uint16_t idx;
    (void)unused1;
    (void)unused2;

    idx = *(const uint16_t *)rec;
    if (idx >= 300u) {
        rearlight_log_event(RL_LOG_SRC_OVERFLOW, 0xb2, 0);
        return -1;
    }
    {
        unsigned int off = (unsigned int)idx * 0xcu;
        *(volatile uint16_t *)(RL_FRAME_TABLE + off) =
            (uint16_t)((uint16_t)rec[2] | ((uint16_t)rec[3] << 8));
        mem_copy((void *)(RL_FRAME_TABLE + off + 2u), rec + 4, 0xb);
    }
    return 0;
}
