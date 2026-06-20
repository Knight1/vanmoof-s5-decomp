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
 * 32-bit result to the RAM table at ESH_REF_SAMPLES. Three iterations (offsets 0x6c,
 * 0x6e, 0x70).
 */
void eshifter_sensor_pair_average(int cfg)
{
    int *out = (int *)ESH_REF_SAMPLES;                  /* averaged-result table (RAM) */
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
 * Forms base - (coarse*3600 + fine), where coarse (ESH_TIME_COARSE) is a 32-bit
 * counter, fine (ESH_TIME_FINE) is a SIGN-EXTENDED 16-bit term (OEM uses SXTAH),
 * and base (ESH_TIME_BASE) is a 32-bit base value. Consumed by the run state of
 * eshifter_position_sensor_task_step as the actuator target.
 */
int eshifter_calc_time_offset(void)
{
    int   coarse = *(volatile int *)ESH_TIME_COARSE;
    short fine   = *(volatile short *)ESH_TIME_FINE;   /* signed 16-bit */
    int   base   = *(volatile int *)ESH_TIME_BASE;

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
 *     ESH_MOTOR_BASE+2/+0x19), sets the stop flag (ESH_DRIVE_RAMP=1), clears the torque
 *     command (ESH_MOTOR_CMD=0) and returns.
 *  2. Otherwise it shifts the prev/cur delta pair (ESH_POS_DELTA_PAIR), resets the
 *     position accumulator (ESH_POS_ACCUM) when state.pos changed (ESH_ACTUATOR_LASTPOS).
 *  3. pos_acc += (prev+delta)/2, clamped to [-20000, 19999].
 *  4. Feed-forward iq from the velocity term (acc*50 >> 8, clamped 3000), plus
 *     (delta*200)/12, clamped to [-8000, 8000].
 *  5. Phase machine selects motor direction/enables per sign of iq and the
 *     phase byte, using the dir flags at ESH_DIR_FLAG_REV (reverse) / ESH_DIR_FLAG_FWD
 *     (forward).
 *  6. Drive output: soft-start ramp via the dwell counter (ESH_DRIVE_RAMP), clamp
 *     |iq| to 9999, write torque command (ESH_MOTOR_CMD = iq*dir) and PWM duty
 *     ((iq*0x12bf)/10000 -> ESH_PWM_BASE+0x7c).
 */
void eshifter_actuator_drive_step(unsigned char *state, int target)
{
    int  *pos_pair = (int *)ESH_POS_DELTA_PAIR;     /* [0]=cur delta, [1]=prev */
    int  *last_pos = (int *)ESH_ACTUATOR_LASTPOS;
    int  *pos_acc  = (int *)ESH_POS_ACCUM;
    int   statepos = *(int *)(state + 4);
    int   delta;
    int   prev;
    int   acc;
    int   v;
    int   iq;
    int   dir;
    unsigned char phase;
    unsigned short *ramp = (unsigned short *)ESH_DRIVE_RAMP;

    delta = target - statepos;
    if ((unsigned)(delta + 9) < 0x13) {
        if (*state != 1) {
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 2)    = 0;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 1;
        }
        *state = 1;
        *(volatile unsigned short *)ESH_DRIVE_RAMP = 1;   /* stop flag */
        *(volatile int *)ESH_MOTOR_CMD = 0;
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
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 3)    = 0;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 2)    = 1;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 1;
            *state = 2;
            break;
        case 2:
            *state = 2;
            break;
        case 3:
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 2) = 0;
            *(volatile short *)ESH_DIR_FLAG_REV = 0;
            *state = 4;
            break;
        case 4:
            if (*(volatile short *)ESH_DIR_FLAG_REV == 0) {
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 2) = 0;
                *(volatile short *)ESH_DIR_FLAG_REV = 0;
                *state = 4;
            } else {
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 3)    = 0;
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 2)    = 1;
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 1;
                *(volatile unsigned short *)ESH_DRIVE_RAMP = 1;
                *state = 2;
            }
            break;
        default:
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 0;
            *state = 0;
            break;
        }
    } else {
        dir = 1;
        switch (phase) {
        case 0:
        case 1:
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 3)    = 1;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 2)    = 1;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 1;
            *state = 3;
            break;
        case 2:
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 2) = 0;
            *(volatile int *)ESH_DIR_FLAG_FWD = 0;
            *state = 4;
            break;
        case 3:
            *state = 3;
            break;
        case 4:
            if (*(volatile int *)ESH_DIR_FLAG_FWD == 0) {
                *(volatile int *)ESH_DIR_FLAG_FWD = 1;
                *state = 4;
            } else {
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 3)    = 1;
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 2)    = 1;
                *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 1;
                *(volatile unsigned short *)ESH_DRIVE_RAMP = 1;
                *state = 3;
            }
            break;
        default:
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 0;
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
    *(volatile int *)ESH_MOTOR_CMD = iq * dir;
    *(volatile unsigned int *)(ESH_PWM_BASE + 0x7c) =
        (unsigned)(iq * 0x12bf) / 10000;     /* PWM duty */
}

/* eshifter_position_sensor_task_step — one iteration of the position-sensor
 * processing + drive state machine.
 *
 * OEM disassembly (0x000025a4..0x00002a1a):
 *
 * (a) Read six signed ADC samples (ESH_ADC_BASE+0x300), storing the value as u16
 *     when negative else 0.
 * (b) Ratiometric difference: diff[i] = (samp[i] - ref[i]) / ref[i] over the
 *     four reference samples at ESH_REF_SAMPLES.
 * (c) Inline rational atan2 (poly coeff 0.28) of (diff0+diff2, diff1+diff3) ->
 *     a 14-bit electrical angle/position (scaled by 65536/2pi, biased 0x2000).
 * (d) Position-delta wrap counter at ESH_POS_WRAP_IDX (clamped 0..14).
 * (e) Tick helper (eshifter_tick_update) + a second wrap tracker over
 *     ESH_POS_RAW_CUR/ESH_POS_RAW_PREV into the 32-bit counter ESH_TIME_COARSE; pushes the
 *     wrapped delta into the 128-entry ring at ESH_DELTA_RING; writes the fine time
 *     term (ESH_TIME_FINE) consumed by eshifter_calc_time_offset.
 * (f) Single-pole IIR (b0=0.01546, a1=0.96906) on samp[5] into the float ring
 *     at ESH_IIR_FILT_RING (u16 input ring at ESH_IIR_INPUT_RING).
 * (g) Calibration FSM (enable byte ESH_CALIB_ENABLE, state byte ESH_CALIB_FSM):
 *     case 0 collects per-channel max/min calibration accumulators until 1200
 *     samples; case 1 stores the calibration into the config record (magic
 *     ESH_CALIB_MAGIC) and persists it; case 2 runs the actuator; default resets.
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
        int adc = *(volatile int *)(ESH_ADC_BASE + 0x300);
        samp[i] = (adc < 0) ? (unsigned short)adc : 0;
    }

    /* (b) ratiometric differences */
    mem_set(diff, 0, 0x10);
    {
        const int *ref = (const int *)ESH_REF_SAMPLES;
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
                unsigned short *poscell = (unsigned short *)ESH_POS_LAST14;
                signed char    *widx    = (signed char *)ESH_POS_WRAP_IDX;
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
        unsigned short *cur   = (unsigned short *)ESH_POS_RAW_CUR;
        unsigned short *old   = (unsigned short *)ESH_POS_RAW_PREV;
        int            *wrapc = (int *)ESH_TIME_COARSE;
        unsigned short  v     = *cur;
        unsigned short  o     = *old;
        int             d2;
        unsigned char  *ridx  = (unsigned char *)ESH_DELTA_RING_IDX;
        int            *ring  = (int *)ESH_DELTA_RING;
        unsigned char   bi;

        *old = v;
        d2 = (int)v - (int)o;
        if (d2 < -0x8000)
            *wrapc = *wrapc + 1;
        else if (d2 > 0x8000)
            *wrapc = *wrapc - 1;
        *(volatile unsigned short *)ESH_TIME_FINE =
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
        unsigned char  *fidx  = (unsigned char *)ESH_IIR_RING_IDX;
        unsigned short *ring2 = (unsigned short *)ESH_IIR_INPUT_RING;
        float          *filt  = (float *)ESH_IIR_FILT_RING;
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
        signed char    enable = *(signed char *)ESH_CALIB_ENABLE;
        unsigned char *fsm    = (unsigned char *)ESH_CALIB_FSM;
        if (enable == 0)
            return;
        mem_copy((void *)ESH_SAMPLE_COPY, samp, 0xc);

        switch (*fsm) {
        case 0: {
            unsigned short *countA = (unsigned short *)ESH_CALIB_COUNT_A;
            unsigned short *countB = (unsigned short *)ESH_CALIB_COUNT_B;
            unsigned short *endA   = (unsigned short *)ESH_CALIB_COUNT_B;
            int *maxA = (int *)ESH_CALIB_MAX_A;
            int *minB = (int *)ESH_CALIB_MIN_B;
            int *accA = (int *)ESH_CALIB_ACC_A;
            int *accB = (int *)ESH_CALIB_ACC_B;
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
            int   *actuator = (int *)ESH_ACTUATOR_PTR;
            int    act = *actuator;
            int    rec;
            const int *srcA = (const int *)ESH_CALIB_ACC_A;
            const int *srcB = (const int *)ESH_CALIB_ACC_B;
            unsigned short *p;

            eshifter_actuator_dead_stop((unsigned char *)act);
            *(int *)(act + 4) = 0;
            *(int *)ESH_MOTOR_CMD = 0;
            eshifter_calib_reset();

            rec = *(int *)ESH_CONFIG_REC_PTR;            /* config record */
            p = (unsigned short *)(rec + 0x74);
            do {
                ++p;
                p[0] = (unsigned short)*srcA;
                p[4] = (unsigned short)*srcB;
                ++srcA; ++srcB;
            } while ((unsigned short *)(rec + 0x7c) != p);
            *(unsigned short *)(rec + 0x88) = ESH_CALIB_MAGIC;   /* calibration magic */
            eshifter_write_config_record((uint8_t *)rec);
            eshifter_sensor_pair_average(rec + 0xa);
            *fsm = 2;
            break;
        }
        case 2: {
            int act = *(int *)ESH_ACTUATOR_PTR;
            int tgt = eshifter_calc_time_offset();
            eshifter_actuator_drive_step((unsigned char *)act, tgt);
            break;
        }
        case 3:
            break;
        default: {
            int act = *(int *)ESH_ACTUATOR_PTR;
            *(volatile unsigned char *)(ESH_MOTOR_BASE + 0x19) = 0;
            *(int *)(act + 4) = 0;
            *(unsigned char *)act = 0;
            break;
        }
        }
    }
}
