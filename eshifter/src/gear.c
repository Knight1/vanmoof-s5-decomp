/*
 * gear.c — eshifter gear-shift FSM, actuator drive and position sensing.
 *
 * VanMoof S5/A5 electronic gear-shifter controller. Translated from the OEM
 * image (NXP LPC546xx Cortex-M4F, image base 0x0). The SCTimer/PWM + motor
 * driver register pokes are NXP-SDK shaped but the control arithmetic, the
 * rational atan2 angle stage, the position/wrap tracking and the calibration
 * state machine are VanMoof; MMIO/RAM accesses are done verbatim against the
 * device addresses (volatile literal-address casts).
 */

#include "eshifter.h"

/* eshifter_sensor_pair_average — average adjacent 16-bit position-sensor samples
 * into a result table.
 *
 * OEM disassembly (0x00000420..0x0000043e):
 *
 * Helper for the position-sensor task's calibration-store path. The pointer
 * starts at cfg+0x6a and is pre-incremented by one halfword before each read,
 * so the first read is at cfg+0x6c; it sums *p (low table) and p[4] (the partner
 * 8 bytes ahead), arithmetic-shifts the 17-bit sum right by one and stores the
 * 32-bit result to the RAM table at 0x200001b8. Three iterations (offsets 0x6c,
 * 0x6e, 0x70).
 */
void eshifter_sensor_pair_average(int cfg)
{
    int *out = (int *)0x200001b8;                  /* averaged-result table (RAM) */
    unsigned short *p = (unsigned short *)(cfg + 0x6a);

    do {
        ++p;                                       /* first read at cfg+0x6c */
        *out = (int)((unsigned)p[0] + (unsigned)p[4]) >> 1;  /* signed >>1 (ASR) */
        ++out;
    } while ((unsigned short *)(cfg + 0x72) != p);
}

/* eshifter_calc_time_offset — compute the commanded target offset.
 *
 * OEM disassembly (0x00000444..0x0000045c):
 *
 * Forms base - (coarse*3600 + fine), where coarse (0x20000758) is a 32-bit
 * counter, fine (0x20000f76) is a SIGN-EXTENDED 16-bit term (OEM uses SXTAH),
 * and base (0x20000e54) is a 32-bit base value. Consumed by the run state of
 * eshifter_position_sensor_task_step as the actuator target.
 */
int eshifter_calc_time_offset(void)
{
    int   coarse = *(volatile int *)0x20000758;
    short fine   = *(volatile short *)0x20000f76;   /* signed 16-bit */
    int   base   = *(volatile int *)0x20000e54;

    return base - (coarse * 0xe10 + (int)fine);
}

/* eshifter_actuator_drive_step — gear-actuator motor drive sequencing.
 *
 * OEM disassembly (entry 0x000005f0..0x00000692, drive-output tail
 * 0x000004e4..0x000005c8, dead-stop helper 0x000004c8..0x000004dc — Ghidra
 * fuses the three ranges via internal tail-branches):
 *
 * state -> actuator struct (state[0] = phase byte 0..4, *(int*)(state+4) =
 * last commanded position); target = commanded target.
 *  1. delta = target - state.pos. If (delta+9U) < 0x13 (the -9..+9 on-target
 *     dead-band) it parks the drive (phase=1, clear motor-enable bytes at
 *     0x4008c000+2/+0x19), sets the stop flag (0x20000f7c=1), clears the torque
 *     command (0x200001cc=0) and returns.
 *  2. Otherwise it shifts the prev/cur delta pair (0x200006c8), resets the
 *     position accumulator (0x200006c4) when state.pos changed (0x200006d8).
 *  3. pos_acc += (prev+delta)/2, clamped to [-20000, 19999].
 *  4. Feed-forward iq from the velocity term (acc*50 >> 8, clamped 3000), plus
 *     (delta*200)/12, clamped to [-8000, 8000].
 *  5. Phase machine selects motor direction/enables per sign of iq and the
 *     phase byte, using the dir flags at 0x20000f7e (reverse) / 0x20000f80
 *     (forward).
 *  6. Drive output: soft-start ramp via the dwell counter (0x20000f7c), clamp
 *     |iq| to 9999, write torque command (0x200001cc = iq*dir) and PWM duty
 *     ((iq*0x12bf)/10000 -> 0x40028000+0x7c).
 */
void eshifter_actuator_drive_step(unsigned char *state, int target)
{
    int  *pos_pair = (int *)0x200006c8;     /* [0]=cur delta, [1]=prev */
    int  *last_pos = (int *)0x200006d8;
    int  *pos_acc  = (int *)0x200006c4;
    int   statepos = *(int *)(state + 4);
    int   delta;
    int   prev;
    int   acc;
    int   v;
    int   iq;
    int   dir;
    unsigned char phase;
    unsigned short *ramp = (unsigned short *)0x20000f7c;

    delta = target - statepos;
    if ((unsigned)(delta + 9) < 0x13) {
        if (*state != 1) {
            *(volatile unsigned char *)(0x4008c000 + 2)    = 0;
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 1;
        }
        *state = 1;
        *(volatile unsigned short *)0x20000f7c = 1;   /* stop flag */
        *(volatile int *)0x200001cc = 0;
        return;
    }

    prev          = pos_pair[0];
    pos_pair[0]   = delta;
    pos_pair[1]   = prev;
    if (statepos != *last_pos) {
        *last_pos = statepos;
        *pos_acc  = 0;
    }

    acc = *pos_acc + (prev + delta) / 2;
    if (acc > 19999)
        acc = 19999;
    if (acc < -20000)
        acc = -20000;
    *pos_acc = acc;

    v = acc * 0x32;                          /* 50 */
    if (v >= -768255) {                       /* OEM literal 0xfff44701 */
        if (v < 0)
            v += 0xff;
        iq = v >> 8;
        if (iq > 2999)
            iq = 3000;
    } else {
        iq = -3000;
    }
    iq += (delta * 200) / 0xc;
    if (iq > 7999)
        iq = 8000;
    if (iq < -8000)
        iq = -8000;

    /* ---- phase machine + drive output (tail @0x4e4) ---- */
    phase = *state;
    if (iq == 0) {
        dir = 1;                             /* enabled, no phase change */
    } else if (iq < 0) {
        iq  = -iq;
        dir = -1;
        switch (phase) {
        case 0:
        case 1:
            *(volatile unsigned char *)(0x4008c000 + 3)    = 0;
            *(volatile unsigned char *)(0x4008c000 + 2)    = 1;
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 1;
            *state = 2;
            break;
        case 2:
            *state = 2;
            break;
        case 3:
            *(volatile unsigned char *)(0x4008c000 + 2) = 0;
            *(volatile short *)0x20000f7e = 0;
            *state = 4;
            break;
        case 4:
            if (*(volatile short *)0x20000f7e == 0) {
                *(volatile unsigned char *)(0x4008c000 + 2) = 0;
                *(volatile short *)0x20000f7e = 0;
                *state = 4;
            } else {
                *(volatile unsigned char *)(0x4008c000 + 3)    = 0;
                *(volatile unsigned char *)(0x4008c000 + 2)    = 1;
                *(volatile unsigned char *)(0x4008c000 + 0x19) = 1;
                *(volatile unsigned short *)0x20000f7c = 1;
                *state = 2;
            }
            break;
        default:
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 0;
            *state = 0;
            break;
        }
    } else {
        dir = 1;
        switch (phase) {
        case 0:
        case 1:
            *(volatile unsigned char *)(0x4008c000 + 3)    = 1;
            *(volatile unsigned char *)(0x4008c000 + 2)    = 1;
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 1;
            *state = 3;
            break;
        case 2:
            *(volatile unsigned char *)(0x4008c000 + 2) = 0;
            *(volatile int *)0x20000f80 = 0;
            *state = 4;
            break;
        case 3:
            *state = 3;
            break;
        case 4:
            if (*(volatile int *)0x20000f80 == 0) {
                *(volatile int *)0x20000f80 = 1;
                *state = 4;
            } else {
                *(volatile unsigned char *)(0x4008c000 + 3)    = 1;
                *(volatile unsigned char *)(0x4008c000 + 2)    = 1;
                *(volatile unsigned char *)(0x4008c000 + 0x19) = 1;
                *(volatile unsigned short *)0x20000f7c = 1;
                *state = 3;
            }
            break;
        default:
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 0;
            *state = 0;
            break;
        }
    }

    /* ---- drive output (LAB_00000536) ---- */
    {
        unsigned c = *ramp;
        if (c != 0) {
            unsigned short nc = (unsigned short)(c + 1);
            if ((int)(c * 100) <= iq)
                iq = (int)(c * 100);          /* soft-start ramp limit */
            if (((c + 1) & 0xffff) > 100)
                nc = 0;
            *ramp = nc;
        }
    }
    if (iq > 9950)
        iq = 9999;
    *(volatile int *)0x200001cc = iq * dir;
    *(volatile unsigned int *)(0x40028000 + 0x7c) =
        (unsigned)(iq * 0x12bf) / 10000;     /* PWM duty */
}

/* eshifter_position_sensor_task_step — one iteration of the position-sensor
 * processing + drive state machine.
 *
 * OEM disassembly (0x000025a4..0x00002a1a):
 *
 * (a) Read six signed ADC samples (0x400a0000+0x300), storing the value as u16
 *     when negative else 0.
 * (b) Ratiometric difference: diff[i] = (samp[i] - ref[i]) / ref[i] over the
 *     four reference samples at 0x200001b8.
 * (c) Inline rational atan2 (poly coeff 0.28) of (diff0+diff2, diff1+diff3) ->
 *     a 14-bit electrical angle/position (scaled by 65536/2pi, biased 0x2000).
 * (d) Position-delta wrap counter at 0x20000f8a (clamped 0..14).
 * (e) Tick helper (eshifter_tick_update) + a second wrap tracker over
 *     0x20000f7a/0x20000f78 into the 32-bit counter 0x20000758; pushes the
 *     wrapped delta into the 128-entry ring at 0x200001d0; writes the fine time
 *     term (0x20000f76) consumed by eshifter_calc_time_offset.
 * (f) Single-pole IIR (b0=0.01546, a1=0.96906) on samp[5] into the float ring
 *     at 0x2000049c (u16 input ring at 0x20000e68).
 * (g) Calibration FSM (enable byte 0x20000f89, state byte 0x20000158):
 *     case 0 collects per-channel max/min calibration accumulators until 1200
 *     samples; case 1 stores the calibration into the config record (magic
 *     0x8550) and persists it; case 2 runs the actuator; default resets.
 *
 * The decompiler's coprocessor_function2/NAN/in_fpscr terms are VFP-compare
 * (vcmpe/vmrs) artifacts and carry no logic.
 */
void eshifter_position_sensor_task_step(void)
{
    unsigned short samp[6];
    float diff[4];
    int   i;

    /* (a) read six ADC samples */
    for (i = 0; i < 6; ++i) {
        int adc = *(volatile int *)(0x400a0000 + 0x300);
        samp[i] = (adc < 0) ? (unsigned short)adc : 0;
    }

    /* (b) ratiometric differences */
    mem_set(diff, 0, 0x10);
    {
        const int *ref = (const int *)0x200001b8;
        for (i = 0; i < 4; ++i) {
            float r = (float)ref[i];
            float x = (float)(unsigned)samp[i];
            diff[i] = (x - r) / r;
        }
    }

    /* (c) inline atan2 angle -> 14-bit electrical position */
    {
        const float PI          = 3.14159274f;
        const float HALF_PI     = 1.57079637f;
        const float NEG_HALF_PI = -1.57079637f;
        const float K           = 0.280000001f;
        const float TWO_PI      = 6.28318548f;
        const float SCALE       = 65536.0f;
        float num = diff[0] + diff[2];
        float den = diff[1] + diff[3];
        float ang;

        if (den != 0.0f) {
            float r = num / den;
            float a = fabsf(r);
            if (a >= 1.0f) {
                ang = HALF_PI - r / (r * r * K + 1.0f);
                if (num < 0.0f)
                    ang -= PI;
            } else {
                ang = r / (r * r * K + 1.0f);
                if (den < 0.0f) {
                    if (num < 0.0f)
                        ang -= PI;
                    else
                        ang += PI;
                }
            }
        } else {
            ang = (num >= 0.0f) ? HALF_PI : NEG_HALF_PI;
        }

        {
            unsigned u   = (unsigned)(((ang + PI) * SCALE) / TWO_PI);
            int      pos = (int)(u & 0xffff) - 0x2000;
            unsigned pos14;
            if (pos < 0)
                pos14 = (u & 0xffff) + 0xe000;
            else
                pos14 = (unsigned)pos;

            /* (d) position delta / wrap counter */
            {
                unsigned short *poscell = (unsigned short *)0x20000f68;
                signed char    *widx    = (signed char *)0x20000f8a;
                unsigned short  prev    = *poscell;
                int d;
                *poscell = (unsigned short)pos14;
                d = (int)(pos14 & 0xffff) - (int)prev;
                if (d < -0x8000)
                    *widx = (signed char)(*widx + 1);
                else if (d > 0x8000)
                    *widx = (signed char)(*widx - 1);
                if (*widx < 0)
                    *widx = 0x0e;
                else if (*widx > 0x0e)
                    *widx = 0;
            }
        }
    }

    /* (e) tick helper + second wrap tracker + delta ring buffer */
    eshifter_tick_update();
    {
        unsigned short *cur   = (unsigned short *)0x20000f7a;
        unsigned short *old   = (unsigned short *)0x20000f78;
        int            *wrapc = (int *)0x20000758;
        unsigned short  v     = *cur;
        unsigned short  o     = *old;
        int             d2;
        unsigned char  *ridx  = (unsigned char *)0x20000f88;
        int            *ring  = (int *)0x200001d0;
        unsigned char   bi;

        *old = v;
        d2 = (int)v - (int)o;
        if (d2 < -0x8000)
            *wrapc = *wrapc + 1;
        else if (d2 > 0x8000)
            *wrapc = *wrapc - 1;
        *(volatile unsigned short *)0x20000f76 =
            (unsigned short)((unsigned)v * 0xe10 >> 16);
        if (d2 < -0x8000)
            d2 += 0x10000;
        else if (d2 > 0x8000)
            d2 -= 0x10000;
        bi = *ridx;
        ring[bi] = d2;
        *ridx = (unsigned char)((bi + 1) & 0x7f);
    }

    /* (f) IIR filter on samp[4] */
    {
        const float b0 = 0x1.facc22p-7f;   /* OEM 0x3c7d6611 */
        const float a1 = 0x1.f0299ep-1f;   /* OEM 0x3f7814cf */
        unsigned char  *fidx  = (unsigned char *)0x20000f82;
        unsigned short *ring2 = (unsigned short *)0x20000e68;
        float          *filt  = (float *)0x2000049c;
        unsigned        idx   = *fidx;
        unsigned        pidx  = (idx == 0) ? 0x7f : ((idx - 1) & 0xff);
        float           x     = (float)(unsigned)samp[4];
        float           yprev = (float)(unsigned)ring2[pidx];
        ring2[idx] = samp[4];
        filt[idx]  = yprev * b0 + x * b0 + filt[pidx] * a1;
        *fidx = (unsigned char)((idx + 1) & 0x7f);
    }

    /* (g) calibration FSM */
    {
        signed char    enable = *(signed char *)0x20000f89;
        unsigned char *fsm    = (unsigned char *)0x20000158;
        if (enable == 0)
            return;
        mem_copy((void *)0x20000f6a, samp, 0xc);

        switch (*fsm) {
        case 0: {
            unsigned short *countA = (unsigned short *)0x20000e58;
            unsigned short *countB = (unsigned short *)0x20000e60;
            unsigned short *endA   = (unsigned short *)0x20000e60;
            int *maxA = (int *)0x20000130;
            int *minB = (int *)0x20000140;
            int *accA = (int *)0x2000010c;
            int *accB = (int *)0x2000011c;
            unsigned short *s     = samp;
            unsigned short  total = 0;
            unsigned char   next  = *fsm;
            int             advance = 0;

            do {
                unsigned short ca = *countA;
                if (ca < 0x96) {                 /* A-side max tracker -> 40000 */
                    int mx = *maxA;
                    int x  = (int)*s;
                    if (mx < x) {
                        *maxA = x;
                    } else if ((mx - x) > 10000 && mx > 40000) {
                        int a = *accA;
                        if (ca == 0)      a = mx;
                        else if (ca == 1) a = (a + mx) >> 1;
                        else              a = mx + ((a - mx) >> 2);
                        *accA = a;
                        *countA = ca + 1;
                        *maxA = 40000;
                    }
                }
                {
                    unsigned short cb = *countB;
                    if (cb < 0x96) {             /* B-side min tracker -> 25000 */
                        int x  = (int)*s;
                        int mn = *minB;
                        if (x < mn) {
                            *minB = x;
                        } else if ((x - mn) > 10000 && mn < 25000) {
                            int a = *accB;
                            if (cb == 0)      a = mn;
                            else if (cb == 1) a = (a + mn) >> 1;
                            else              a = mn + ((a - mn) >> 2);
                            *accB = a;
                            *countB = cb + 1;
                            *minB = 25000;
                        }
                    }
                    total = (unsigned short)(*countA + *countB + total);
                }
                if (total >= 0x4b0) {            /* 1200: collection complete */
                    next = 1;
                    advance = 1;
                }
                ++accB; ++minB; ++s; ++accA; ++maxA; ++countB;
                ++countA;
            } while (endA != countA);

            if (advance)
                *fsm = next;
            break;
        }
        case 1: {
            int   *actuator = (int *)0x200006a0;
            int    act = *actuator;
            int    rec;
            const int *srcA = (const int *)0x2000010c;
            const int *srcB = (const int *)0x2000011c;
            unsigned short *p;

            eshifter_actuator_dead_stop((unsigned char *)act);
            *(int *)(act + 4) = 0;
            *(int *)0x200001cc = 0;
            eshifter_calib_reset();

            rec = *(int *)0x2000069c;            /* config record */
            p = (unsigned short *)(rec + 0x74);
            do {
                ++p;
                p[0] = (unsigned short)*srcA;
                p[4] = (unsigned short)*srcB;
                ++srcA; ++srcB;
            } while ((unsigned short *)(rec + 0x7c) != p);
            *(unsigned short *)(rec + 0x88) = 0x8550;   /* calibration magic */
            eshifter_write_config_record((uint8_t *)rec);
            eshifter_sensor_pair_average(rec + 0xa);
            *fsm = 2;
            break;
        }
        case 2: {
            int act = *(int *)0x200006a0;
            int tgt = eshifter_calc_time_offset();
            eshifter_actuator_drive_step((unsigned char *)act, tgt);
            break;
        }
        case 3:
            break;
        default: {
            int act = *(int *)0x200006a0;
            *(volatile unsigned char *)(0x4008c000 + 0x19) = 0;
            *(int *)(act + 4) = 0;
            *(unsigned char *)act = 0;
            break;
        }
        }
    }
}
