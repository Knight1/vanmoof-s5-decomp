/*
 * Small HAL helpers.
 *
 * Translated from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS). Behaviour-faithful reconstruction
 * of the raw disassembly.
 */

#include "hal.h"

/*
 * Lookup table consumed by irqn_to_gpio_index, read verbatim from OEM rodata
 * at 0x0000a510 (36 bytes = nine 32-bit little-endian words). The search
 * pointer is an int* stepped by one element (4 bytes) per iteration.
 *
 *   0000a510: 00 48 00 48  00 48 00 48  00 40 00 28  00 10 08 01
 *   0000a520: 00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00
 *   0000a530: 00 00 00 00
 */
static const int32_t kIrqnTable[9] = {
    0x48004800, /* 0000a510 */
    0x48004800, /* 0000a514 */
    0x28004000, /* 0000a518 */
    0x01081000, /* 0000a51c */
    0x00000000, /* 0000a520 */
    0x00000000, /* 0000a524 */
    0x00000000, /* 0000a528 */
    0x00000000, /* 0000a52c */
    0x00000000, /* 0000a530 */
};

/*
 * irqn_to_gpio_index // 0x000020e4
 *
 *   000020e4: mov   r3,r0            ; r3 = key (param_1)
 *   000020e6: movs  r0,#0x0          ; index = 0  (return value in r0)
 *   000020e8: ldr   r2,[kIrqnTable]
 *   000020ea: ldr.w r1,[r2],#0x4     ; r1 = *p++  (post-increment by 4)
 *   000020ee: cmp   r1,r3
 *   000020f0: beq   0x000020f8       ; match -> return current index
 *   000020f2: adds  r0,#0x1          ; ++index
 *   000020f4: cmp   r0,#0x9
 *   000020f6: bne   0x000020ea       ; loop while index != 9
 *   000020f8: bx    lr               ; returns index in r0
 *
 * Note: on no match the loop exits with index == 9 (one past the table), and
 * that value is returned. Callers only pass keys that are present, so this
 * out-of-range result is never observed in practice.
 */
int irqn_to_gpio_index(int irqn)
{
    int index = 0;
    const int32_t *p = kIrqnTable;

    do {
        if (*p == irqn) {
            return index;
        }
        index++;
        p++;
    } while (index != 9);

    return index;
}

/*
 * SCG (System Clock Generator) base. The OEM literal pool at 0x000011bc
 * holds 0x40020000; the function indexes it at +0x98 and +0xb8.
 */
#define SCG_BASE     0x40020000U
#define SCG_REG_98   (*(volatile uint32_t *)(SCG_BASE + 0x98)) /* 0x40020098 */
#define SCG_REG_B8   (*(volatile uint32_t *)(SCG_BASE + 0xb8)) /* 0x400200b8 */

/*
 * GetClock_32k // 0x00001188
 *
 *   00001188: ldr   r3,[SCG_BASE]
 *   0000118a: ldr.w r2,[r3,#0xb8]
 *   0000118e: lsls  r0,r2,#0x19      ; test bit 6 of [0xb8]
 *   00001190: bmi   0x0000119a       ; bit6 set -> second test
 *   00001192: ldr.w r2,[r3,#0x98]
 *   00001196: lsls  r1,r2,#0x1f      ; test bit 0 of [0x98]
 *   00001198: bpl   0x000011b2       ; bit0 clear -> return 0x8000
 *   0000119a: ldr.w r2,[r3,#0xb8]
 *   0000119e: lsls  r2,r2,#0x18      ; test bit 7 of [0xb8]
 *   000011a0: bmi   0x000011b8       ; bit7 set -> return 0
 *   000011a2: ldr.w r0,[r3,#0x98]
 *   000011a6: ands  r0,r0,#0x1       ; bit0 of [0x98]
 *   000011aa: it    ne
 *   000011ac: mov.ne.w r0,#0x8000    ; -> 0x8000 if bit0 set, else r0 (==0)
 *   000011b0: bx    lr
 *   000011b2: mov.w r0,#0x8000
 *   000011b6: bx    lr
 *   000011b8: movs  r0,#0x0
 *   000011ba: bx    lr
 */
uint32_t GetClock_32k(void)
{
    if (((SCG_REG_B8 & (1U << 6)) == 0) && ((SCG_REG_98 & (1U << 0)) == 0)) {
        return 0x8000U;
    }

    if ((SCG_REG_B8 & (1U << 7)) == 0) {
        return (SCG_REG_98 & 1U) != 0 ? 0x8000U : 0U;
    }

    return 0U;
}
