/*
 * q16.c - Q16.16 signed fixed-point math library.
 *
 * Translated from VanMoof S5/A5 user_ecu firmware
 * (user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin), ARM Cortex-M4F.
 *
 * Each function keeps its exact OEM name and ABI. Logic mirrors the
 * decompiled/disassembled implementation; behaviour-faithful, not
 * byte-for-byte identical.
 */
#include "q16.h"

#include <stdint.h>
#include <stdbool.h>

/*
 * q16_mul - saturating Q16.16 multiply.
 * // 0x0000835e
 *
 * Computes a*b in Q16.16 with round-to-nearest (+0x8000 before the >>16).
 * Magnitudes are multiplied with a 16x16 -> 64-bit-ish expansion; the sign is
 * applied at the end. If the magnitude does not fit (top bit of the high word
 * set after rounding) the result saturates to 0x80000000.
 */
q16_t q16_mul(q16_t a, q16_t b)
{
    uint32_t sa = (uint32_t)((int32_t)a >> 31);
    uint32_t sb = (uint32_t)((int32_t)b >> 31);

    /* Absolute values via the conditional-negate idiom (x ^ s) - s. */
    uint32_t ma = ((uint32_t)a ^ sa) - sa;
    uint32_t mb = ((uint32_t)b ^ sb) - sb;

    uint32_t ah = ma >> 16;
    uint32_t al = ma & 0xffff;
    uint32_t bh = mb >> 16;
    uint32_t bl = mb & 0xffff;

    uint32_t mid = bl * ah + al * bh;     /* cross terms */
    uint32_t lo = bl * al;                /* low product */
    uint32_t hi = bh * ah + (mid >> 16);  /* high product */

    uint32_t mid_lo = mid << 16;
    uint32_t sum = lo + mid_lo;
    if (sum < lo) {            /* carry out of the low add (CARRY4) */
        hi += 1;
    }

    uint32_t result;
    if ((hi >> 15) == 0) {
        if (sum > 0xffff7fff) {   /* rounding (+0x8000) carries into hi */
            hi += 1;
        }
        result = ((sum + 0x8000) >> 16) | (hi << 16);
        if ((uint32_t)(-(int32_t)sb) != (uint32_t)(-(int32_t)sa)) {
            result = (uint32_t)(-(int32_t)result);
        }
    } else {
        result = 0x80000000u;     /* overflow */
    }
    return (q16_t)result;
}

/*
 * q16_div - Q16.16 restoring division (a / b).
 * // 0x000083b8
 *
 * Returns 0x80000000 when the divisor is zero or on overflow. Operates on
 * magnitudes via a normalise-then-restoring-divide loop, applying the sign
 * at the end.
 */
q16_t q16_div(q16_t a, q16_t b)
{
    if (b == 0) {
        return (q16_t)0x80000000u;
    }

    uint32_t bit = 0x10000u;
    uint32_t sa = (uint32_t)((int32_t)a >> 31);
    uint32_t sb = (uint32_t)((int32_t)b >> 31);
    uint32_t rem = ((uint32_t)a ^ sa) - sa;   /* |a| */
    uint32_t div = ((uint32_t)b ^ sb) - sb;   /* |b| */

    /* Normalise divisor up so it spans the dividend. */
    for (; div < rem; div <<= 1) {
        bit <<= 1;
    }

    if (bit == 0) {
        return (q16_t)0x80000000u;
    }

    if ((int32_t)div < 0) {
        div >>= 1;
        bit >>= 1;
    }

    uint32_t quot = 0;
    for (; bit != 0; bit >>= 1) {
        if (rem == 0) {
            goto done;
        }
        if (div <= rem) {
            rem -= div;
            quot |= bit;
        }
        rem <<= 1;
    }
    if (div <= rem) {
        quot += 1;
    }

done:
    if ((uint32_t)(-(int32_t)sb) == (uint32_t)(-(int32_t)sa)) {
        return (q16_t)quot;
    }
    if (quot == 0x80000000u) {
        return (q16_t)0x80000000u;
    }
    return (q16_t)(-(int32_t)quot);
}

/*
 * q16_sqrt - Q16.16 base-4 restoring square root.
 * // 0x0000841e
 *
 * Two-phase digit-by-digit (base-4) restoring sqrt: first the integer part,
 * then a fractional refinement that yields the Q16.16 result.
 */
q16_t q16_sqrt(q16_t x)
{
    uint32_t v = (uint32_t)x;
    uint32_t bit;
    uint32_t root;
    uint32_t t;

    /* Find the highest base-4 digit position not exceeding the input. */
    for (bit = 0x40000000u; v < bit; bit >>= 2) {
    }

    root = 0;
    for (; bit != 0; bit >>= 2) {
        t = bit + root;
        root >>= 1;
        if (t <= v) {
            v -= t;
            root += bit;
        }
    }

    bit = 0x4000u;
    uint32_t acc = root * 0x10000u;
    if (v < 0x10000u) {
        v <<= 16;
    } else {
        v = (v - root) * 0x10000u - 0x8000u;
        acc += 0x8000u;
    }

    do {
        t = bit + acc;
        acc >>= 1;
        if (t <= v) {
            v -= t;
            acc += bit;
        }
        bit >>= 2;
    } while (bit != 0);

    if (acc < v) {
        acc += 1;
    }
    return (q16_t)acc;
}

/*
 * ============================ TABLE ANOMALY WARNING ========================
 * The q16_exp factor tables below are read VERBATIM from this firmware image at
 * 0x0000a4e0 (neg) / 0x0000a4f0 (pos) - the pointers (literal pool @0xb58/0xb5c)
 * and the 32-bit element stride (ldr.w r1,[r7,r5,lsl #2]) are both verified.
 *
 * HOWEVER these values are NOT valid natural-exp multipliers: tab[0] should be
 * e^1.0 == 0x0002B7E1, but the image holds 0x10002800. The clamp bounds DO match
 * natural exp (0xa65ae = +10.38 ~= ln(0x7fff); 0xfff4376e = -11.78), so the
 * function IS exp() structurally - the rodata at 0xa4e0 in THIS dump does not
 * contain the runtime table (consistent with the image's other anomalies: decoy
 * vector table, VTOR=0xc00 to code, off-image data >0x1a88b). q16_exp / q16_sigmoid
 * are therefore behaviour-UNVERIFIED until the real table is recovered. See
 * docs/progress.md and ghidra/exports/user_ecu_program.json open items.
 * ==========================================================================
 */

/* // 0x0000a4e0 - factors used when the input is negative (ANOMALOUS, see above) */
static const q16_t q16_exp_tab_neg[4] = {
    0x30001800, /* 0x0000a4e0 */
    0x48004800, /* 0x0000a4e4 */
    0x48004800, /* 0x0000a4e8 */
    0x48004800, /* 0x0000a4ec */
};

/* // 0x0000a4f0 - factors used when the input is non-negative (ANOMALOUS, see above) */
static const q16_t q16_exp_tab_pos[4] = {
    0x10002800, /* 0x0000a4f0 */
    0x18000021, /* 0x0000a4f4 */
    0x48003000, /* 0x0000a4f8 */
    0x48004800, /* 0x0000a4fc */
};

/*
 * q16_exp - Q16.16 natural exponential exp(x).
 * // 0x00000b04
 *
 * Input is clamped: above DAT_00000b50 (0x000a65ae) the result saturates to
 * 0x7fffffff; below DAT_00000b54 (0xfff4376e, signed -0x000bc892) it underflows
 * to 0. Otherwise the magnitude is decomposed against the step sizes
 * { 0x10000, 0x2000, 0x400, 0x80 } and the accumulator is repeatedly multiplied
 * by the matching table factor (negative table for x < 0, positive otherwise).
 */
q16_t q16_exp(q16_t x)
{
    /* DAT_00000b50 / DAT_00000b54 (read from the OEM literal pool). */
    const int32_t clamp_hi = 0x000a65ae; /* // 0x00000b50 */
    const int32_t clamp_lo = (int32_t)0xfff4376e; /* // 0x00000b54 */

    if (x > clamp_hi) {
        return (q16_t)0x7fffffff;
    }
    if (x < clamp_lo) {
        return 0;
    }

    bool neg = (x < 0);
    int32_t mag = neg ? -x : x;

    q16_t acc = Q16_ONE;
    int32_t step = Q16_ONE;
    const q16_t *tab = neg ? q16_exp_tab_neg : q16_exp_tab_pos;

    int idx = 0;
    do {
        for (; step <= mag; mag -= step) {
            acc = q16_mul(acc, tab[idx]);
        }
        idx += 1;
        step >>= 3;
    } while (idx != 4);

    return acc;
}

/*
 * q16_sigmoid - Q16.16 logistic 1/(1 + exp(gain*(x - x0))).
 * // 0x0000847c
 *
 * Reads the gain from self+0x6c and the offset x0 from self+0x70, forms
 * t = gain * (x - x0) (saturating multiply), then clamps:
 *   t < -0x320000 (-50.0) -> returns 0x10000 (1.0)
 *   t >  0x320000 (+50.0) -> returns 0
 * otherwise returns q16_div(0x10000, q16_exp(t) + 0x10000).
 */
q16_t q16_sigmoid(const void *self, q16_t x)
{
    const uint8_t *base = (const uint8_t *)self;
    q16_t gain = *(const q16_t *)(base + 0x6c);
    q16_t x0 = *(const q16_t *)(base + 0x70);

    q16_t t = q16_mul(gain, x - x0);

    if (t < -0x320000) {
        return Q16_ONE;
    }
    if (t > 0x320000) {
        return 0;
    }

    q16_t e = q16_exp(t);
    return q16_div(Q16_ONE, e + Q16_ONE);
}
