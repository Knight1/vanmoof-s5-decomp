#ifndef USER_ECU_UTIL_H
#define USER_ECU_UTIL_H

/*
 * util.h — VanMoof firmware low-level utilities.
 *
 * The firmware ships its own hand-written byte-fill / byte-copy / byte-compare
 * (memset/memcpy/memcmp-equivalents at 0x9866/0x984c/0x982c) and a counted
 * busy-wait, rather than relying on libc/libgcc. They are used widely across the
 * image (e.g. the I2C descriptor zero-fill, xTaskCreate buffer init, the
 * registry entry copy, device-record readback verify, SystemInit, and
 * clock-stabilisation delays).
 */

#include <stddef.h>
#include <stdint.h>

/*
 * vmem_set — fill `count` bytes at `dst` with byte `value`. // 0x00009866
 *
 * OEM ABI: (dst, value, count); returns void (no value in r0 on exit).
 * The OEM body precomputes end = dst + count, then writes one byte at a time
 * with a post-increment store, terminating on pointer equality (dst == end).
 * Unlike a toolchain memset it is byte-granular, has no word/alignment fast
 * path, and does not return dst.
 */
void vmem_set(void *dst, uint8_t value, size_t count);

/*
 * vmem_copy — copy `count` bytes from `src` to `dst`, forward. // 0x0000984c
 *
 * OEM ABI: (dst, src, count). The hand-written sibling of vmem_set: it
 * precomputes end = src + count, then copies one byte at a time (ldrb/strb
 * post/pre-increment) terminating on src == end. Byte-granular forward copy
 * with no overlap handling and no word/alignment fast path; returns void.
 * Used by registry_add to stamp a 0x2c-byte entry into a slot.
 */
void vmem_copy(void *dst, const void *src, size_t count);

/*
 * vmem_cmp — compare `count` bytes of `a` and `b`, forward. // 0x0000982c
 *
 * OEM ABI: (a, b, count). The third hand-written libc-trio sibling: precomputes
 * end = a + count, compares one byte at a time terminating on a == end, and
 * returns 0 when all equal or the signed difference (int)a[i] - (int)b[i] of the
 * first mismatch. Byte-granular, no word/alignment fast path; not a toolchain
 * memcmp. Used by the I²C write-then-verify path and device-record readback.
 */
int vmem_cmp(const void *a, const void *b, size_t count);

/*
 * mem_free — free `p` if non-NULL. // 0x000087f2
 *
 * OEM ABI: (p); returns void. NULL-guarded tail-call to the FreeRTOS heap_4
 * vPortFree (0x6b9c, vendor). The firmware's generic allocation-release used
 * across the I²C/bus session layer (session open/commit cleanup, etc.).
 */
void mem_free(void *p);

/*
 * busy_wait — spin for `count` decrements. // 0x000084b2
 *
 * OEM ABI: (count); returns void. A bare `do { count--; } while (count != 0);`
 * delay loop (subs/cmp/bne). Note count == 0 on entry underflows and spins
 * 2^32 iterations — reproduced verbatim. Used for clock-stabilisation delays.
 */
void busy_wait(uint32_t count);

#endif /* USER_ECU_UTIL_H */
