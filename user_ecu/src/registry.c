/*
 * registry.c — fixed-stride entry registry with comparator-based lookup.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   registry_find @ 0x000082c6  (linear comparator scan)
 *   registry_add  @ 0x00008594  (dedup + append)
 *
 * Entries are fixed 0x2c-byte records held in `reg->slots`; the comparator at
 * `reg->cmp` decides membership against a key located 0xc bytes into a record.
 */

#include "registry.h"
#include "util.h"   /* vmem_copy */

/*
 * registry_find — index of the first slot whose key matches. // 0x000082c6
 *
 * OEM disassembly (0x82c6..0x82f0):
 *   push {r3..r7,lr}
 *   mov  r5,r0 ; mov r6,r1        ; r5 = reg, r6 = key
 *   movs r4,#0x0                  ; i = 0
 *   movs r7,#0x2c                 ; entry stride
 * loop:
 *   ldr  r3,[r5,#0x0]             ; count
 *   cmp  r3,r4
 *   bhi  hit                      ; while (count > i)
 *   mov.w r4,#0xffffffff          ; exhausted -> -1
 *   mov  r0,r4 ; pop {..,pc}
 * hit:
 *   ldrd r3,r1,[r5,#0x8]          ; r3 = cmp, r1 = slots
 *   mov  r0,r6                    ; r0 = key
 *   mla  r1,r7,r4,r1              ; r1 = slots + i*0x2c
 *   blx  r3                       ; cmp(key, slot)
 *   cmp  r0,#0x0
 *   beq  out (return i)           ; match
 *   adds r4,#0x1 ; b loop
 *
 * Note the comparator is invoked with only r0/r1 deliberately set (key, slot);
 * the registry header's count/capacity are not forwarded.
 */
uint32_t registry_find(registry_t *reg, const void *key)
{
    uint32_t i = 0;

    while (i < reg->count) {                         /* cmp count,i ; bhi */
        uint8_t *slot = reg->slots + i * REGISTRY_ENTRY_SIZE; /* mla */
        if (reg->cmp(key, slot) == 0) {              /* blx ; cmp #0 ; beq */
            return i;                                /* match */
        }
        i = i + 1;                                   /* adds r4,#1 */
    }
    return 0xffffffffu;                              /* not found */
}

/*
 * registry_add — append `entry` unless full or a duplicate. // 0x00008594
 *
 * OEM disassembly (0x8594..0x85ca):
 *   push {r3,r4,r5,lr}
 *   mov  r5,r1 ; mov r4,r0        ; r5 = entry, r4 = reg
 *   cbnz r0,check                 ; reg != NULL ?
 *   mov.w r0,#0xffffffff ; pop    ; NULL -> -1
 * check:
 *   ldrd r2,r3,[r0,#0x0]          ; r2 = count, r3 = capacity
 *   cmp  r2,r3 ; bcs fail         ; full (count >= capacity) -> -1
 *   adds r1,#0xc                  ; key = entry + 0xc
 *   bl   registry_find
 *   adds r0,#0x1 ; bne fail       ; found (!= -1) -> -1
 *   ldr  r3,[r4,#0x0]             ; count
 *   ldr  r0,[r4,#0xc]             ; slots
 *   adds r2,r3,#0x1 ; str r2,[r4] ; count = count + 1
 *   movs r2,#0x2c                 ; copy size
 *   mov  r1,r5                    ; src = entry
 *   mla  r0,r2,r3,r0              ; dst = slots + count*0x2c
 *   bl   vmem_copy ; movs r0,#0x0 ; copy, return 0
 */
uint32_t registry_add(registry_t *reg, const void *entry)
{
    uint32_t index;

    if (reg == (registry_t *)0) {                    /* cbnz r0 */
        return 0xffffffffu;
    }

    if (reg->count >= reg->capacity) {               /* cmp ; bcs */
        return 0xffffffffu;
    }

    /* Reject if an entry with the same key (entry + 0xc) already exists. */
    if (registry_find(reg, (const uint8_t *)entry + REGISTRY_KEY_OFFSET)
            != 0xffffffffu) {                        /* adds r0,#1 ; bne */
        return 0xffffffffu;
    }

    index = reg->count;                              /* ldr r3,[r4] */
    reg->count = index + 1;                          /* str count+1 */
    vmem_copy(reg->slots + index * REGISTRY_ENTRY_SIZE, entry,
              REGISTRY_ENTRY_SIZE);                  /* mla dst ; bl 0x984c */
    return 0;
}
