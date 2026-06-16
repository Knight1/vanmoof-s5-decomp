/*
 * util.c — VanMoof firmware low-level utilities.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 *
 * Functions:
 *   vmem_set     @ 0x00009866  (hand-written byte fill / memset)
 *   vmem_copy    @ 0x0000984c  (hand-written forward byte copy / memcpy)
 *   vmem_cmp     @ 0x0000982c  (hand-written byte compare / memcmp)
 *   vmem_strncmp @ 0x00009876  (hand-written bounded NUL-aware compare / strncmp)
 *   busy_wait    @ 0x000084b2  (counted spin delay)
 *   mem_free     @ 0x000087f2  (free-if-non-NULL wrapper over vPortFree)
 */

#include "util.h"

/* FreeRTOS heap_4 free (vendor, deferred) — 0x00006b9c. */
extern void vPortFree(void *pv);

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
 * vmem_cmp — firmware byte compare (memcmp). // 0x0000982c
 *
 * OEM disassembly (0x982c..0x984a):
 *   0000982c  subs r1,#0x1          ; r1 = b - 1 (pre-increment cursor)
 *   0000982e  add  r2,r0            ; r2 = end = a + count
 *   00009830  push {r4,lr}
 *   00009832  cmp  r0,r2            ; loop top: a == end ?
 *   00009834  bne  0x0000983a
 *   00009836  movs r0,#0x0          ; all bytes equal -> 0
 *   00009838  b    0x00009846
 *   0000983a  ldrb r3,[r0,#0x0]     ; r3 = *a
 *   0000983c  ldrb.w r4,[r1,#0x1]!  ; r4 = *++b
 *   00009840  cmp  r3,r4
 *   00009842  beq  0x00009848
 *   00009844  subs r0,r3,r4         ; first mismatch -> *a - *b
 *   00009846  pop  {r4,pc}
 *   00009848  adds r0,#0x1          ; a++ ; b advanced by the pre-increment ldrb
 *   0000984a  b    0x00009832
 *
 * Third member of the hand-written libc trio (cmp/copy/set at 0x982c/0x984c/
 * 0x9866): forward byte compare, end-pointer terminator via a-pointer equality,
 * returning 0 when equal or the signed difference of the first differing byte
 * pair. Byte-granular, no word/alignment fast path; NOT a toolchain/libgcc
 * memcmp. Used by the write-then-verify path (bus_page_write_verify) and the
 * device-record readback compare.
 */
int vmem_cmp(const void *a, const void *b, size_t count)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;     // b - 1, pre-incremented to b
    const uint8_t *end = pa + count;            // add r2,r0

    while (pa != end) {                         // cmp r0,r2 ; bne
        if (*pa != *pb) {                       // ldrb ; ldrb ; cmp ; bne
            return (int)*pa - (int)*pb;         // subs r0,r3,r4
        }
        pa = pa + 1;                            // adds r0,#1
        pb = pb + 1;
    }
    return 0;                                   // movs r0,#0
}

/*
 * vmem_strncmp — firmware bounded NUL-aware byte compare (strncmp). // 0x00009876
 *
 * OEM disassembly (0x9876..0x989a):
 *   00009876  push {r4,lr}
 *   00009878  cbz  r2,0x00009896      ; n == 0 -> return 0
 *   0000987a  mov  r3,r0              ; p1 = s1
 *   0000987c  subs r1,#0x1            ; p2 = s2 - 1 (pre-increment cursor)
 *   0000987e  adds r4,r0,r2           ; end = s1 + n
 *   00009880  ldrb.w r0,[r3],#0x1     ; c1 = *p1 ; p1++
 *   00009884  ldrb.w r2,[r1,#0x1]!    ; p2++ ; c2 = *p2
 *   00009888  cmp  r0,r2              ; mismatch ?
 *   0000988a  bne  0x00009890
 *   0000988c  cmp  r3,r4              ; reached s1 + n ?
 *   0000988e  bne  0x00009898
 *   00009890  subs r0,r0,r2           ; return c1 - c2
 *   00009892  pop  {r4,pc}
 *   00009896  movs r0,r2              ; (n == 0) r0 = 0
 *   00009898  cmp  r0,#0x0            ; loop while c1 != 0 (stop at NUL)
 *   0000989a  bne  0x00009880  /  beq 0x00009890
 *
 * Fourth member of the hand-written string/mem family (after the cmp/copy/set
 * trio at 0x982c/0x984c/0x9866): a forward bounded compare that also stops at a
 * shared NUL — i.e. strncmp semantics. Byte-granular with the house pre-increment
 * idiom on the second operand (s2 - 1, advanced before each load); returns 0 when
 * equal (or n == 0) and the signed difference of the first differing pair
 * otherwise. NOT a toolchain/libc strncmp (same byte-granular, no fast path).
 */
int vmem_strncmp(const char *s1, const char *s2, size_t n)
{
    unsigned int c1;
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2 - 1;    /* subs r1,#1 */
    const uint8_t *end = p1 + n;                    /* adds r4,r0,r2 */

    if (n == 0) {
        return 0;                                   /* movs r0,r2 (==0) */
    }

    do {
        c1 = *p1;                                   /* ldrb.w r0,[r3],#1 */
        p1 = p1 + 1;
        p2 = p2 + 1;                                /* ldrb.w r2,[r1,#1]! */
        if (c1 != *p2 || p1 == end) {               /* cmp/bne ; cmp r3,r4 */
            break;
        }
    } while (c1 != 0);                              /* cmp r0,#0 ; bne */

    return (int)c1 - (int)*p2;                      /* subs r0,r0,r2 */
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

/*
 * mem_free — free `p` if non-NULL. // 0x000087f2
 *
 * OEM disassembly (0x87f2..0x8800):
 *   000087f2  cbz  r0,0x000087fc   ; p == NULL ? return
 *   000087f4  ...  b.w 0x00006b9c  ; tail-call vPortFree(p)
 *   000087fc  bx   lr
 *
 * Thin VanMoof wrapper that guards the firmware's free path with a NULL check
 * before tail-calling the FreeRTOS heap_4 vPortFree (0x6b9c, vendor). Used as
 * the generic "release this allocation" across the I²C/bus session layer.
 */
void mem_free(void *p)
{
    if (p != (void *)0) {           // cbz r0
        vPortFree(p);               // b.w 0x6b9c
    }
}
