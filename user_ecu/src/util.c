/*
 * util.c — VanMoof firmware low-level utilities.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 *
 * Functions:
 *   vmem_set  @ 0x00009866  (hand-written byte fill / memset)
 *   vmem_copy @ 0x0000984c  (hand-written forward byte copy / memcpy)
 *   busy_wait @ 0x000084b2  (counted spin delay)
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

/*
 * vmem_copy — firmware forward byte-copy (memcpy). // 0x0000984c
 *
 * OEM disassembly (0x984c..0x9864):
 *   0000984c  add  r2,r1            ; r2 = end = src + count
 *   0000984e  subs r3,r0,#0x1       ; r3 = dst - 1 (pre-increment cursor)
 *   00009850  cmp  r1,r2            ; src == end ? (count == 0)
 *   00009852  bne  0x00009856
 *   00009854  bx   lr               ; nothing to copy
 *   00009856  push {r4,lr}
 *   00009858  ldrb.w r4,[r1],#0x1   ; r4 = *src++
 *   0000985c  cmp  r1,r2
 *   0000985e  strb.w r4,[r3,#0x1]!  ; *++dstcur = r4  (first store lands at dst)
 *   00009862  bne  0x00009858
 *   00009864  pop  {r4,pc}
 *
 * Hand-written sibling of vmem_set: byte-granular forward copy, end-pointer
 * terminator via src-pointer equality, no overlap handling, no word fast path.
 * NOT a toolchain/libgcc memcpy, so reproduced here as VanMoof util.
 */
void vmem_copy(void *dst, const void *src, size_t count)
{
    const uint8_t *cursor = (const uint8_t *)src;
    const uint8_t *end = cursor + count;    // add r2,r1
    uint8_t *out = (uint8_t *)dst;          // dst - 1, pre-incremented to dst

    while (cursor != end) {                 // cmp r1,r2 ; bne
        *out = *cursor;                     // ldrb then strb
        out = out + 1;
        cursor = cursor + 1;
    }
}

/*
 * busy_wait — counted spin delay. // 0x000084b2
 *
 * OEM disassembly (0x84b2..0x84bc):
 *   000084b2  mov  r0,r0            ; alignment nop
 *   000084b4  sub.w r0,r0,#0x1      ; loop: count--
 *   000084b8  cmp  r0,#0x0
 *   000084ba  bne  0x000084b4
 *   000084bc  bx   lr
 *
 * Decrement-until-zero busy loop. A count of 0 on entry underflows and spins
 * the full 2^32 iterations (the OEM does not guard for it).
 */
void busy_wait(uint32_t count)
{
    do {
        count = count - 1;          // sub.w r0,r0,#0x1
    } while (count != 0);           // cmp r0,#0x0 ; bne
}
