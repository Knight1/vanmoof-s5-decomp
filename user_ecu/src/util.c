/*
 * util.c — VanMoof firmware byte-fill utility.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 *
 * Functions:
 *   vmem_set @ 0x00009866  (hand-written byte fill / memset)
 */

#include "util.h"

/*
 * vmem_set — firmware byte-fill (memset). // 0x00009866
 *
 * OEM disassembly (0x9866..0x9875):
 *   00009866  add  r2,r0          ; r2 = end = dst + count
 *   00009868  mov  r3,r0          ; r3 = cursor = dst
 *   0000986a  cmp  r3,r2          ; loop top: cursor == end ?
 *   0000986c  bne  0x00009870
 *   0000986e  bx   lr             ; done (returns void)
 *   00009870  strb.w r1,[r3],#0x1 ; *cursor++ = (uint8_t)value
 *   00009874  b    0x0000986a
 *
 * This is the firmware's own fill loop: byte-granular, end-pointer terminator
 * via pointer equality, no alignment/word fast path, and no return value
 * (r0 is reused as the running base and never restored to dst before bx lr).
 * It is NOT a toolchain/libgcc memset, so it is reproduced here as VanMoof util.
 *
 * The OEM passes `value` as a byte (strb of r1); the count is a plain integer
 * span (add r2,r0). Loop terminates immediately when count == 0 (cursor == end
 * on entry).
 */
void vmem_set(void *dst, uint8_t value, size_t count)
{
    uint8_t *cursor = (uint8_t *)dst;
    uint8_t *end = cursor + count;

    while (cursor != end) {
        *cursor = value;        // strb.w r1,[r3],#0x1
        cursor = cursor + 1;
    }
}
