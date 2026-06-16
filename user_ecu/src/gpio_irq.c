/*
 * gpio_irq.c — GPIO interrupt-source registration glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Single function: gpio_irq_register (Ghidra FUN_0000159c). It is invoked
 * 4x from the system-init routine (0x552c/0x5538/0x5544/0x5550) to fill the
 * RAM registration table at 0x20000668 — one 12-byte slot per source, for
 * pins 0x16 / 0x15 / 0x05 / 0x0f.
 */

#include <stdint.h>

#include "gpio_irq.h"
#include "util.h"   /* vmem_set */

/*
 * Base of the GPIO-IRQ registration table in RAM. Loaded verbatim from the
 * literal pool at 0x000015dc. The table holds (at least) 4 slots of 0x0c
 * bytes; the caller pre-clears the first 0x30 bytes before registering.
 */
#define GPIO_IRQ_TABLE_BASE 0x20000668u  /* 0x20000668 */

/* FreeRTOS heap_4 allocator (0x00006a10) — vendor, not reconstructed. */
extern void *pvPortMalloc(uint32_t size);

/* gpio_irq_register // 0x0000159c */
void gpio_irq_register(uint8_t bank, uint8_t pin, uint8_t idx, uint8_t seq)
{
    /* slot = table_base + seq * 0x0c */
    uint8_t *table = (uint8_t *)GPIO_IRQ_TABLE_BASE;
    uint8_t *slot = table + (uint32_t)seq * 0xc; /* 0xc */

    /* Zero the 12-byte slot (mul r10,#0xc; bl 0x00009866). */
    vmem_set(slot, 0, 0xc); /* 0xc */

    /* Descriptor bytes (strb.w r9/r8 ; strb r7). */
    slot[1] = bank; /* +0x01 */
    slot[2] = pin;  /* +0x02 */
    slot[3] = idx;  /* +0x03 */

    /* Slot index, written as a byte at +0 (strb.w r5,[r11,r10]). */
    table[(uint32_t)seq * 0xc] = seq; /* +0x00 */

    /* Allocate a 12-byte work block and stash its pointer at +0x04. */
    *(void **)(slot + 4) = pvPortMalloc(0xc); /* 0xc, +0x04 */
}
