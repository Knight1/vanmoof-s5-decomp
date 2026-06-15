/*
 * q16.h - Q16.16 signed fixed-point math library.
 *
 * Translated from VanMoof S5/A5 user_ecu firmware
 * (user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin), ARM Cortex-M4F.
 *
 * Fixed-point format is signed Q16.16: 1.0 == 0x00010000.
 */
#ifndef Q16_H
#define Q16_H

#include <stdint.h>

/* Signed Q16.16 fixed-point type. */
typedef int32_t q16_t;

/* 1.0 in Q16.16. */
#define Q16_ONE 0x00010000

/* Saturating Q16.16 multiply (round-to-nearest, overflow -> 0x80000000). */
q16_t q16_mul(q16_t a, q16_t b);

/* Q16.16 restoring division (a / b); b == 0 or overflow -> 0x80000000). */
q16_t q16_div(q16_t a, q16_t b);

/* Q16.16 base-4 restoring square root. */
q16_t q16_sqrt(q16_t x);

/* Q16.16 natural exponential exp(x); input clamped, table-driven.
 * NOTE: the factor tables read from this image (0xa4e0/0xa4f0) are anomalous
 * (see q16.c) - the algorithm is faithful but the numerics are UNVERIFIED. */
q16_t q16_exp(q16_t x);

/*
 * Q16.16 logistic 1/(1 + exp(k*(x - x0))).
 * 'self' points to an object whose field at +0x6c is the gain (q16_t) and
 * field at +0x70 is the offset x0 (q16_t). 'x' is the input value.
 * Depends on q16_exp -> same table caveat applies.
 */
q16_t q16_sigmoid(const void *self, q16_t x);

#endif /* Q16_H */
