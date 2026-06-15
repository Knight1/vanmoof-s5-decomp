/*
 * pcc.c — peripheral bring-up glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * All MMIO is reproduced verbatim from the OEM image as volatile accesses at
 * absolute addresses; the originating instruction address is noted inline.
 */

#include "pcc.h"

#include <stdint.h>

/* Peripheral clock-gate / PORT controller block base. */
#define PCC_BLOCK_BASE 0x40000000u

/* NVIC Interrupt Set-Enable Register bank (ISER0). // literal @ 0x000078ec */
#define NVIC_ISER_BASE 0xE000E100u

/* GPIO bank base addresses. // literal @ 0x00002014 */
#define GPIO_BANK0_BASE 0x40082000u
#define GPIO_BANK1_BASE (GPIO_BANK0_BASE + 0x25000u) /* 0x400A7000 */

/*
 * pcc_gate_enable — enable a peripheral clock gate from a packed index.
 * // 0x00008a64
 */
void pcc_gate_enable(uint32_t packed_index)
{
    /* word_index = (idx >> 8) + 0x88; register = base + word_index*4 */
    uint32_t word_index = (packed_index >> 8) + 0x88u; /* 0x00008a66/8a72 */
    volatile uint32_t *reg =
        (volatile uint32_t *)(PCC_BLOCK_BASE + word_index * 4u); /* 0x8a6e/8a74 */

    *reg = 1u << (packed_index & 0xffu); /* 0x00008a68/8a6a/8a74 */
}

/*
 * pcc_gate_set — conditionally latch a clock-gate bit. // 0x00008aca
 */
void pcc_gate_set(volatile uint32_t *block_base, uint32_t gate_index)
{
    uint32_t mask = 1u << (gate_index & 0xffu); /* 0x00008aca/8ace */

    /* block_base[9] == offset 0x24. Latch only when the bit is currently
     * clear in [0] but reported set in [0x24]. // 0x00008acc..8adc */
    if (((block_base[0] & mask) == 0u) && ((mask & block_base[9]) != 0u)) {
        block_base[9] = mask;
    }
}

/*
 * port_clock_wait — PORT clock-gate enable + ready handshake. // 0x000084cc
 */
void port_clock_wait(uint32_t packed_index)
{
    uint32_t mask = 1u << (packed_index & 0xffffu); /* uxth; 0x84ce/84d0 */
    uint32_t off = (packed_index >> 0x10) * 4u;      /* 0x000084d2/84d4 */

    volatile uint32_t *enable_set =
        (volatile uint32_t *)(PCC_BLOCK_BASE + 0x120u + off); /* 0x84da */
    volatile uint32_t *status =
        (volatile uint32_t *)(PCC_BLOCK_BASE + 0x100u + off); /* 0x84de */
    volatile uint32_t *enable_clear =
        (volatile uint32_t *)(PCC_BLOCK_BASE + 0x140u + off); /* 0x84e6 */

    *enable_set = mask;                       /* 0x000084da */
    while ((mask & *status) == 0u) {          /* 0x000084de..84e4 */
    }
    *enable_clear = mask;                     /* 0x000084e6 */
    while ((mask & ~*status) == 0u) {         /* 0x000084ea..84f2 */
    }
}

/*
 * nvic_irq_enable — set the NVIC ISER bit for an IRQ number. // 0x000078d4
 */
void nvic_irq_enable(uint32_t irq)
{
    if ((int32_t)irq >= 0) { /* 0x000078d4/78d6 */
        volatile uint32_t *iser =
            (volatile uint32_t *)(NVIC_ISER_BASE + (irq >> 5) * 4u); /* 0x78da/78e4/78e6 */
        *iser = 1u << (irq & 0x1fu); /* 0x000078dc/78e0/78e6 */
    }
}

/*
 * gpio_base_to_bank — map a GPIO bank base address to a 0/1/2 index.
 * // 0x00001ffc
 */
uint32_t gpio_base_to_bank(uint32_t bank_base)
{
    if (bank_base == GPIO_BANK0_BASE) { /* 0x00001ffc..2000 */
        return 0u;
    }
    if (bank_base == GPIO_BANK1_BASE) { /* 0x00002002..200a */
        return 1u;
    }
    return 2u; /* 0x0000200c */
}
