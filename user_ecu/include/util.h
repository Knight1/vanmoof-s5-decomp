#ifndef USER_ECU_UTIL_H
#define USER_ECU_UTIL_H

/*
 * util.h — VanMoof firmware byte-fill utility.
 *
 * The firmware ships its own hand-written byte-fill (memset-equivalent) rather
 * than relying on a libc/libgcc memset. It is used widely across the image
 * (e.g. the I2C descriptor zero-fill, xTaskCreate buffer init, SystemInit).
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

#endif /* USER_ECU_UTIL_H */
