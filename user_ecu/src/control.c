/*
 * control.c - VanMoof LED-ring control module.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M VFPv4 hard-float, FreeRTOS).
 *
 * Functions (Ghidra names / addresses):
 *   ledParams_Init           @ 0x00000b60
 *   ledEasing_ControlUpdate  @ 0x00000c64  (logical body 0xc64..0x1032; Ghidra
 *                                           marks only 0xc64..0xd95 because the
 *                                           DECOY reset vector @0x0 points into
 *                                           the middle of the body, splitting it)
 *   controlTask_CmdHandler   @ 0x00001ee0
 *   int_pair_to_float        @ 0x00000884  (VFP {whole,micros}->float helper)
 *
 * All fixed-point math is signed Q16.16 (1.0 == 0x00010000).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "control.h"
#include "q16.h"
#include "protocol.h"

/* ------------------------------------------------------------------------- */
/* Cross-module VanMoof symbols (declared, not reconstructed here).           */
/* These belong to the i2c module (i2c.h); declared locally so control.c      */
/* compiles standalone. Signatures recovered from the disassembly.            */
/* ------------------------------------------------------------------------- */

/* i2c module: send an assembled TX frame. // 0x00007408 */
extern int i2c_tx_frame(void *frame, int len);
/* i2c module: receive + CRC-verify a frame in place. // 0x000073a0 */
extern int i2c_rx_frame_verify(void *frame, unsigned int half_count);
/* i2c module: read the 0xe28 status word into *out (byte-swapped). // 0x00008964 */
extern int i2c_read_status_e28(void *out);

/* Bus/peripheral settle delay (busy-wait helper). // 0x00001bdc */
extern void busyWait_Ticks(uint32_t ticks);

/* SRAM-resident LED-ring channel state structs and the tick counter.
 * Addresses verbatim from the controlTask_CmdHandler literal pool
 * (0x00001ff0/0x00001ff4/0x00001ff8). */
extern volatile uint32_t g_ctrl_tick_counter; /* 0x2000080c (*DAT_00001ff0) */
extern LedCtrlState       g_ctrl_struct_A;     /* 0x20001ce4 (DAT_00001ff4)  */
extern LedCtrlState       g_ctrl_struct_B;     /* 0x20000860 (DAT_00001ff8)  */

/* Scaled-sensor source floats (read by the cmd handler over the bus layer).
 * From the controlTask literal pool: DAT_00001fd8 -> 0x20000008,
 * DAT_00001fe4 -> 0x2000000c. */
extern volatile float g_sensor_src_0;          /* *(float*)0x20000008 (*DAT_00001fd8) */
extern volatile float g_sensor_src_1;          /* *(float*)0x2000000c (*DAT_00001fe4) */

/* ------------------------------------------------------------------------- */
/* ledParams_Init - initialise an LED-ring channel state struct.             */
/* 0x00000b60                                                                */
/* ------------------------------------------------------------------------- */
void ledParams_Init(LedCtrlState *state, int variant, q16_t params_a, q16_t params_b)
{
    state->variant = variant;

    if (variant == 1) {
        state->level_base = 0x10000;       /* +0x04 */
        state->cmd_step   = 0x2710;        /* +0x08 == &LAB_00002710 used as literal */
        state->off_0c     = 0x2d00000;     /* +0x0c */
        state->off_10     = 0x42cc0000;    /* +0x10 DAT_00000c44 (float 102.0 bit-pattern) */
        state->off_14     = 0x50280000;    /* +0x14 DAT_00000c48 */
        state->off_18     = 0x1e0000;      /* +0x18 */
    } else {
        state->level_base = 0x640000;      /* +0x04 */
        state->cmd_step   = 0x4e20;        /* +0x08 (20000) */
        state->off_0c     = 0xb40000;      /* +0x0c */
        state->off_10     = 0x0a8c0000;    /* +0x10 DAT_00000c58 */
        state->off_14     = 0x14640000;    /* +0x14 DAT_00000c5c */
        state->off_18     = 0x1540000;     /* +0x18 */
    }

    state->gain_scale = 0xe60000;          /* +0x1c */
    state->off_20     = 0xc0000;           /* +0x20 */
    state->off_24     = 0xc0000;           /* +0x24 */
    state->off_28     = 0x320000;          /* +0x28 */
    state->tick       = 0;                 /* +0x2c */
    state->delta      = 0;                 /* +0x30 */
    state->out_value  = 0;                 /* +0x34 */
    state->off_3c     = 0;                 /* +0x3c */
    state->off_40     = 0;                 /* +0x40 */
    state->out_init   = 0;                 /* +0x38 byte */
    state->off_44     = 0x320000;          /* +0x44 */

    /* +0x48: q16_div(0x2469, DAT_00000c4c) ; DAT_00000c4c == 0x000c0012 */
    state->off_48 = q16_div(0x2469, 0x000c0012);
    /* +0x4c: q16_div(0x48d, DAT_00000c4c) */
    state->off_4c = q16_div(0x48d, 0x000c0012);

    /* +0x50: pointer/handle literal, variant-dependent. */
    if (variant == 1) {
        state->off_50 = 0x6d23;            /* &DAT_00006d23 */
    } else {
        state->off_50 = 0x00186186;        /* DAT_00000c50 */
    }

    state->off_5c = 0;                     /* +0x5c */
    state->off_60 = 0;                     /* +0x60 */
    state->off_54 = 0x68d;                 /* +0x54 */
    state->off_58 = 0;                     /* +0x58 */
    state->off_64 = 0;                     /* +0x64 */
    state->off_68 = 0;                     /* +0x68 */
    state->off_74 = 0x320000;              /* +0x74 */
    state->off_78 = 0;                     /* +0x78 */

    if (variant != 1) {
        state->off_7c = 0xfffffe56;        /* +0x7c DAT_00000c60 (-0x1aa) */
        state->off_80 = 0xd50000;          /* +0x80 */
        state->off_84 = 0x640000;          /* +0x84 */
    } else {
        state->off_7c = (q16_t)(0x68d - 0x923); /* +0x7c == -0x296 (subw r3,r3,#0x923) */
        state->off_80 = 0x02660000;        /* +0x80 DAT_00000c54 */
        state->off_84 = 0x10000;           /* +0x84 */
    }

    state->off_88     = 0xc31;             /* +0x88 */
    state->off_8c     = 0x83;              /* +0x8c */
    state->smooth_init = 0;                /* +0x90 byte */
}

/* ------------------------------------------------------------------------- */
/* ledEasing_ControlUpdate - per-tick Q16.16 easing/control kernel.          */
/* 0x00000c64  (full logical body spans 0xc64..0x1032)                       */
/* ------------------------------------------------------------------------- */
void ledEasing_ControlUpdate(LedCtrlState *s, int target, int *out)
{
    /* Startup ramp gate: hold for the first 0x2d0000 ticks. (0xc68..0xc7a) */
    if (s->tick < 0x2d0001) {
        s->tick += 0x10000;
    } else {
        /* Track the command toward 'target', stepping at most 0x7fff/tick. */
        if ((uint32_t)(target - 1) < 64999U) {              /* 0x9a..0xa2 */
            int cur = s->cmd_step;                          /* [+0x08] */
            int next;
            if (cur < target) {
                next = cur + 0x7fff;
                if (target <= cur + 0x7fff) {
                    next = target;
                }
            } else {
                next = cur + 1;
            }
            s->delta = (next - cur) * 0x10000;              /* [+0x30] */
        }

        int variant = s->variant;                           /* [+0x00] */
        if ((variant == 0) || ((int8_t)s->out_init != 0)) {
            /* Active easing path. */
            int denom;
            if (variant != 1) {
                target = s->off_74;                         /* [+0x74] */
                denom  = -0xdc0000 - target;
            } else {
                denom  = 0x7d00000;
            }

            /* normalized = (delta - off_78) / denom        (0xcc8..0xce4) */
            q16_t norm = q16_div(s->delta - s->off_78, denom);
            q16_t v    = q16_mul(norm, s->gain_scale);      /* [+0x1c] */
            s->out_value = v;

            /* exponent = off_7c * (v - off_80)              (0xce6..0xcf4) */
            q16_t e = q16_mul(s->off_7c, v - s->off_80);

            if (e < -0x320000) {                            /* clamp low */
                v = 0x1f40000;
            } else if (e < 0x320001) {
                if (v < 0) {
                    /* (0xe36..0xe56): r0=level_base, r1=off_84 at entry */
                    q16_t a  = q16_div(s->level_base, s->off_84); /* [+0x04],[+0x84] */
                    q16_t ex = q16_exp(e);
                    q16_t d  = q16_div(0x1f40000, ex + 0x10000);
                    v = q16_mul(a, d);
                } else {
                    q16_t bias;
                    if (s->off_84 == 0x10000) {             /* [+0x84] (0xd14..0xd26) */
                        bias = q16_mul(0x00010083, 0x10000 - s->level_base); /* DAT_00000e6c */
                    } else {                                /* (0xe1e..0xe34) */
                        q16_t t = q16_mul(0x50000, s->level_base);
                        bias = q16_div(0x1f40000 - t, 0x40000);
                    }
                    q16_t ex = q16_exp(e);                  /* (0xd2a..0xd3a) */
                    v = q16_div(bias + 0x1f40000, ex + 0x10000);
                    v = v - bias;
                }
            } else {                                        /* clamp high (0xe5e) */
                v = 0;
            }
            s->out_value = v;
        } else {
            /* variant != 0 and not yet initialised: hold at level_base. */
            s->out_value = s->level_base;                   /* [+0x34] = [+0x04] (0xe62) */
        }

        /* --- smoothing / blending stage (0xd3e..0xdd2) --- */
        q16_t v = s->out_value;                             /* [+0x34] */
        if ((int8_t)s->smooth_init == 0) {                  /* [+0x90] */
            s->smooth_a = v;
            s->smooth_b = v;
            s->smooth_c = v;
            s->smooth_init = 1;
        }

        /* smooth_a = lerp(smooth_a, v, off_88) */
        q16_t c = s->off_88;                                /* [+0x88] */
        q16_t a0 = q16_mul(0x10000 - c, s->smooth_a);
        q16_t a1 = q16_mul(c, v);
        q16_t sa = a0 + a1;
        s->smooth_a = sa;

        /* smooth_b = lerp(smooth_b, v, off_8c) */
        q16_t c2 = s->off_8c;                               /* [+0x8c] */
        q16_t b0 = q16_mul(0x10000 - c2, s->smooth_b);
        q16_t b1 = q16_mul(c2, v);
        q16_t sb = b1 + b0;

        /* difference magnitude |sa - sb| */
        q16_t diff = sa - sb;
        if (diff < 0) {
            diff = sb - sa;
        }
        s->smooth_b = sb;

        /* adaptive blend factor based on exp(-DAT_e70 * diff)  (0xd96..0xdb8) */
        q16_t dm = q16_mul(0xffffcccd, diff);               /* DAT_00000e70 */
        q16_t ex = q16_exp(dm);
        q16_t t  = q16_mul(0x1e00000, ex);
        q16_t bf = q16_div(0x10000, t + 0x150000);

        q16_t c0 = q16_mul(0x10000 - bf, s->smooth_c);
        q16_t cv = q16_mul(bf, v);
        cv = cv + c0;

        if (cv < 0x8000) {                                  /* (0xdd4..0xde4) */
            s->out_value = 0x8000;
        }
        int d30 = s->delta;                                 /* [+0x30] */
        if (0x7fff < cv) {
            s->out_value = cv;
        }
        s->smooth_c = cv;

        /* --- per-tick motion integrator, only while delta > 0 --- */
        if (0 < d30) {
            if ((int8_t)s->out_init == 0) {                 /* (0xdf8..0xe00) */
                s->off_3c = 0;
                s->off_40 = d30;
                s->out_init = 1;
                /* falls through to the shared commit at 0xe02 */
            } else {
                /* (0xe74..0x102c): the big momentum/sigmoid update. */
                int acc = s->off_3c;                        /* [+0x3c] */
                if ((uint32_t)(0x0063ffff + acc) > 0x00c7fffeU) { /* DAT_1034 / DAT_1038 */
                    s->off_40 = s->off_40 + acc;            /* [+0x40] += [+0x3c] */
                    s->off_3c = 0;
                }
                int pos = s->off_40;                        /* [+0x40] */

                /* advance the two sigmoid-input ramps up to DAT_103c. */
                int lim = 0x7ffdffff;                       /* DAT_0000103c */
                if (s->off_60 <= lim) {                     /* [+0x60] */
                    s->off_60 += 0x10000;
                }
                if (s->off_64 <= lim) {                     /* [+0x64] */
                    s->off_64 += 0x10000;
                }

                /* first sigmoid block: gain 0x28f, x0 = off_10  (0xea8..0xec4) */
                s->sig_gain = 0x28f;                        /* [+0x6c] */
                s->sig_x0   = s->off_10;                    /* [+0x70] = [+0x10] */
                int r60 = s->off_60;
                q16_t sg0 = q16_sigmoid(s, r60);

                int o48 = s->off_48;                        /* [+0x48] */
                q16_t term0 = q16_mul(s->off_50 - o48, sg0); /* [+0x50] - [+0x48] */

                int r64 = s->off_64;
                q16_t sg1 = q16_sigmoid(s, r64);
                int o18 = s->off_18;                        /* [+0x18] */
                q16_t term1 = q16_mul(0x1fe0000 - o18, sg1);

                int outv = s->out_value;                    /* [+0x34] */
                /* second sigmoid: gain LAB_0000170a, x0 = (term1 + off_18) */
                s->sig_gain = 0x170a;                       /* [+0x6c] = &LAB_0000170a */
                s->sig_x0   = term1 + o18;                  /* [+0x70] */
                q16_t sg2 = q16_sigmoid(s, outv);
                q16_t m16 = q16_mul(sg2, o48 + term0);
                s->off_58 = m16;                            /* [+0x58] (param_1[0x16]) */

                /* third sigmoid block: gain 0x28f, x0 = off_14  (0xf0a..0xf22) */
                s->sig_gain = 0x28f;                        /* [+0x6c] */
                s->sig_x0   = s->off_14;                    /* [+0x70] = [+0x14] */
                q16_t sg3 = q16_sigmoid(s, r60);
                int o4c = s->off_4c;                        /* [+0x4c] */
                q16_t term2 = q16_mul(s->off_54 - o4c, sg3 - sg0); /* [+0x54] - [+0x4c] */

                q16_t sg4 = q16_sigmoid(s, r64);
                q16_t term3 = q16_mul(0x1fe0000 - o18, sg4);
                s->sig_gain = 0x170a;                       /* [+0x6c] */
                s->sig_x0   = term3 + o18;                  /* [+0x70] */
                q16_t sg5 = q16_sigmoid(s, outv);
                q16_t m5c = q16_mul(sg5, o4c + term2);
                s->off_5c = m5c;                            /* [+0x5c] (param_1[0x17]) */

                /* limit accumulator update (0xf5a..0xf86) */
                q16_t lf = q16_mul(0x10000 - sg2, 0x00014ccd); /* DAT_00001040 */
                lf = q16_mul(0x444, lf - 0x4ccd);
                if (lf + s->off_68 < 0) {                    /* [+0x68] */
                    s->off_68 = 0;
                } else {
                    s->off_68 = lf + s->off_68;
                }

                int o3c = s->off_3c;                         /* [+0x3c] (0xf88) */
                if (s->off_0c < s->off_68) {                 /* [+0x0c] < [+0x68] */
                    s->off_64 = 0;                           /* reset ramp B */
                }

                /* radial/length update (0xf98..0x102a) */
                q16_t r = q16_div((d30 - pos) - o3c, 0x400000);
                int o44 = s->off_44;                         /* [+0x44] */
                q16_t ar = (r < 0) ? -r : r;

                q16_t k;
                if (o44 + ar < 0x5a00001) {
                    k = 0x10000;
                } else {
                    k = q16_div(o44 + ar, 0x5a00000);
                    k = q16_mul(k, k);
                }
                /* sqrt(k * (0x400000 - off_5c))  (0xfc8..0xfd2) */
                q16_t s0 = q16_sqrt(q16_mul(k, 0x400000 - m5c));
                /* (off_44^2 / (0x400000*k)) ... */
                q16_t kk  = q16_mul(0x400000, k);
                q16_t q   = q16_div(o44, kk);
                q16_t p1  = q16_mul(o44, q);
                q16_t q2  = q16_mul(m5c, r);
                q16_t q3  = q16_div(q2, k);
                q16_t p2  = q16_mul(q3, r);
                q16_t s1  = q16_sqrt(p2 + p1);
                q16_t len = q16_mul(s0, s1);
                s->off_44 = len;                             /* [+0x44] (param_1[0x11]) */

                /* off_3c += (off_58 * r) / 0x80000   (0x101a..0x102a) */
                q16_t inc = q16_div(q16_mul(m16, r), 0x80000);
                s->off_3c = inc + o3c;                       /* [+0x3c] (param_1[0xf]) */
                /* b 0x00000e02: falls through to the shared commit */
            }
            /* commit (0xe02..0xe0e): off_74 = off_44 ; off_78 = off_40 + off_3c */
            s->off_74 = s->off_44;                           /* param_1[0x1d] = param_1[0x11] */
            s->off_78 = s->off_40 + s->off_3c;               /* param_1[0x1e] = [+0x40]+[+0x3c] */
        }
    }

    /* Quantize out_value to whole units, signed, via arithmetic shift. */
    int q = s->out_value + 0x8000;                           /* (0xc7c..0xc90) */
    bool neg = (q < 0);
    if (neg) {
        q = (int)0xffff8000 - s->out_value;                  /* DAT_00000e68 */
    }
    q = q >> 16;
    if (neg) {
        q = -q;
    }
    *out = q;
}

/* ------------------------------------------------------------------------- */
/* controlTask_CmdHandler - controlTask command dispatcher.                  */
/* 0x00001ee0                                                                */
/* ------------------------------------------------------------------------- */
int controlTask_CmdHandler(int cmd, uint32_t *result, uint32_t arg, uint32_t ctx)
{
    uint16_t out_b = 0;
    uint32_t out_a = 0;
    uint32_t res_a = 0;
    uint32_t res_b = 0;
    int rc;

    /* Local TX/RX frame buffer (OEM: stack region at [sp+8..]). On entry the
     * OEM pushed the incoming arg/ctx registers here, so the buffer starts as
     * {arg, ctx, ...}; every byte that is actually read below is overwritten
     * first (the header bytes + the CRC appends, or the i2c reply), so these
     * initial contents never affect the result. Reproduced verbatim. */
    union {
        uint8_t  b[16];
        uint16_t h[8];
        uint32_t w[4];
    } frame;
    frame.w[0] = arg;   /* sp+0x8 : pushed param_3 */
    frame.w[1] = ctx;   /* sp+0xc : pushed param_4 */
    frame.w[2] = 0;
    frame.w[3] = 0;

    if ((uint32_t)(cmd - 0x1c) < 2U) {
        /* Compute two scaled sensor counts (verbatim VFP math @0x1f02..0x1f50).
         * scale = 65535.0 (0x477fff00). */
        const float scale = 65535.0f;
        /* uVar5 = (g_sensor_src_0 * 65535) / 100.0 */
        float f_a = (g_sensor_src_0 * scale) / 100.0f;       /* /DAT_00001fe0 */
        /* uVar6 = ((g_sensor_src_1 + 45.0) * 65535) / 175.0 */
        float f_b = ((g_sensor_src_1 + 45.0f) * scale) / 175.0f; /* +DAT_1fe8 / DAT_1fec */

        uint16_t count_a = (uint16_t)(uint32_t)f_a;          /* vcvt.u32.f32 + uxth */
        uint16_t count_b = (uint16_t)(uint32_t)f_b;

        /* Build app header: tag 0x1926 (bytes 0x26,0x19 at frame[+0x8/+0x9]). */
        frame.b[0] = 0x26;
        frame.b[1] = 0x19;

        uint32_t off = frame_append_word_crc((int32_t)(intptr_t)&frame, 2, count_a);
        off = frame_append_word_crc((int32_t)(intptr_t)&frame, (int32_t)off, count_b);

        rc = i2c_tx_frame(&frame, (int)off);
        if (rc == 0) {
            busyWait_Ticks(0x32);
            rc = i2c_rx_frame_verify(&frame, 4);
            if (rc == 0) {
                /* Byte-swap the two reply halfwords (rev16). */
                uint16_t h0 = frame.h[0];
                uint16_t h1 = frame.h[1];
                out_a = (uint32_t)(uint16_t)((h0 << 8) | (h0 >> 8));
                out_b = (uint16_t)((h1 << 8) | (h1 >> 8));
            }
        }

        if (rc != 0) {
            return rc;
        }

        g_ctrl_tick_counter = g_ctrl_tick_counter + 1;
        ledEasing_ControlUpdate(&g_ctrl_struct_A, (int)out_a, (int *)&res_a);
        ledEasing_ControlUpdate(&g_ctrl_struct_B, (int)out_b, (int *)&res_b);

        if (cmd != 0x1c) {
            res_a = res_b;
        }
    } else {
        if (cmd != 0x3a) {
            return 4;
        }
        /* The 0x3a path passes the frame buffer straight to the status reader,
         * which builds its own 0xe28 command internally and writes the
         * byte-swapped reply halfword back. (0x1fb6..0x1fd4) */
        rc = i2c_read_status_e28(&frame);
        if (rc != 0) {
            return rc;
        }
        res_a = frame.h[0];                                  /* ldrh [sp,#0x8] */
    }

    result[0] = res_a;
    result[1] = 0;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* int_pair_to_float - convert a {whole, micros} signed int pair to float.    */
/* 0x00000884                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Returns p[0] + p[1] / 1e6 in IEEE-754 single precision: the first word is
 * the whole part, the second word counts millionths. Both words are converted
 * signed (vcvt.f32.s32) under the live FPSCR rounding mode; the divisor
 * 1.0e6f is the literal at 0x000008a4 (0x49742400). Two call sites in the
 * control task (0x00000520 / 0x0000052e).
 */
float int_pair_to_float(const int32_t *p)
{
    return (float)p[0] + (float)p[1] / 1.0e6f; /* 0x00000884..0x000008a2 */
}
