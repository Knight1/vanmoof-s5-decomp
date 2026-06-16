/*
 * clock.c -- clock / peripheral glue
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS).
 *
 * All MMIO is reproduced verbatim from the OEM image as volatile pointer
 * accesses at the absolute peripheral addresses. The decompiler models the
 * SCG/clock-select registers as named globals (_DAT_40000284 etc.); here they
 * are explicit volatile pointers.
 */

#include <stdint.h>

#include "clock.h"
#include "hal.h" /* GetClock_32k() */
#include "pcc.h" /* pcc_gate_set, nvic_irq_enable */

/* ---- System Clock Generator (SCG) clock-select / status registers ------- */
/* Block at 0x40000000: clock-select 0x280/0x284, LPO status 0xa18.          */
#define SCG_CLKSEL_0    (*(volatile uint32_t *)0x40000280u)
#define SCG_CLKSEL_1    (*(volatile uint32_t *)0x40000284u)
#define SCG_LPO_STAT    (*(volatile uint32_t *)0x40000a18u)

/* SCG status block base 0x40013000 (offsets 0x10, 0x20 used here; the 12 MHz
 * "fast IRC valid" status is the aliased word at 0x40013010, bit 14). */
#define SCG_STAT_BASE   0x40013000u
#define SCG_STAT_0x10   (*(volatile uint32_t *)(SCG_STAT_BASE + 0x10u)) /* == 0x40013010 */
#define SCG_STAT_0x20   (*(volatile uint32_t *)(SCG_STAT_BASE + 0x20u))

/*
 * Core-clock frequency globals computed at PLL/flash bring-up
 * (written by SystemClock_PllFlashInit). RAM, accessed via absolute pointers.
 *   0x20000010 : frequency selected when SCG_CLKSEL_0 == 1  (DAT_00001238)
 *   0x20001654 : frequency selected when SCG_CLKSEL_1 == 1  (DAT_00001240)
 */
#define CORE_CLK_FREQ_SEL0_1  (*(volatile uint32_t *)0x20000010u)
#define CORE_CLK_FREQ_SEL1_1  (*(volatile uint32_t *)0x20001654u)

/* ---- NVIC -------------------------------------------------------------- */
/* ISER base 0xE000E100; +0x180 (i.e. +0x60 words) is ICPR (clear-pending). */
#define NVIC_BASE       0xE000E100u
#define NVIC_ICPR_WORD(n) \
    (*(volatile uint32_t *)(NVIC_BASE + (((uint32_t)(n) >> 5) + 0x60u) * 4u))

/*
 * OEM rodata table of NVIC IRQ numbers indexed by peripheral clock-gate index
 * (one signed byte per gate; negative == "no interrupt"). Located at flash
 * 0x0001b34d, outside the analyzed image window; referenced by absolute
 * address exactly as the OEM literal pool does (DAT_000079b8).
 */
#define PERIPH_IRQ_TABLE  ((const int8_t *)0x0001b34du)

/* ---- Clock-divider programming ----------------------------------------- */
/*
 * Divider-config block base (DAT_0000116c == 0x40020000). The two special
 * selectors 0x3f / 0x3e address single fields inside this block; every other
 * selector addresses a word in the divider-register array below.
 */
#define CLKDIV_CFG_BASE     0x40020000u
#define CLKDIV_CFG_0x98     (*(volatile uint32_t *)(CLKDIV_CFG_BASE + 0x98u))
#define CLKDIV_CFG_0x9C     (*(volatile uint32_t *)(CLKDIV_CFG_BASE + 0x9Cu))

/* Per-source clock-divider register array; word `sel` lives at base + sel*4. */
#define CLKDIV_REG_ARRAY    0x40000260u

/* ---- cross-module externs ---------------------------------------------- */
/* pcc_gate_set (0x8aca) and nvic_irq_enable (0x78d4) come from pcc.h. */

/* 0x0000110c */
void clock_div_program(uint32_t packed)
{
    /*
     * `packed` carries up to two 12-bit divider descriptors (low field first,
     * then bits 23:12). Each descriptor is {selector[7:0], value[11:8]} where
     * the programmed divider is value-1 (the OEM stores `(v>>8)-1`, byte-wide).
     *
     * The loop runs at most twice: pass 1 handles the low descriptor, then the
     * word is shifted right 12 and pass 2 handles the next; it stops early when
     * the remaining bits are zero.
     */
    int pass = 2;                                 /* movs r4,#0x2 */

    while (1) {
        if ((packed & 0xfff) != 0) {
            uint32_t sel = packed & 0xff;         /* uxtb r2,r3 */
            uint8_t  val = (uint8_t)(((packed & 0xfff) >> 8) - 1);

            if (sel == 0x3f) {
                /* divider field: bit 0 of cfg+0x98 */
                CLKDIV_CFG_0x98 = (CLKDIV_CFG_0x98 & ~0x1u) | (val & 0x1u);
            } else if (sel == 0x3e) {
                /* divider field: bits 5:4 of cfg+0x9c */
                CLKDIV_CFG_0x9C = (CLKDIV_CFG_0x9C & ~0x30u) | ((val & 0x3u) << 4);
            } else {
                /* divider register array word: base + sel*4 */
                *(volatile uint32_t *)(CLKDIV_REG_ARRAY + sel * 4u) = val & 0xffu;
            }
        }

        if (pass == 1) {
            break;                                /* second descriptor done */
        }
        packed = packed >> 0xc;                   /* advance to next field */
        pass = 1;
        if (packed == 0) {
            return;                               /* nothing left */
        }
    }
}

/* 0x00001170 */
uint32_t Adc_ReadCh_LPO1MHz(void)
{
    uint32_t freq = 0;
    if ((SCG_LPO_STAT & 0x40u) != 0) {
        freq = 1000000u; /* 0x000F4240, DAT_00001184 */
    }
    return freq;
}

/* 0x000011c0 */
uint32_t GetSystemCoreClockSource(void)
{
    if (SCG_CLKSEL_1 == 1) {
        return CORE_CLK_FREQ_SEL1_1; /* *(uint32_t *)0x20001654 */
    }
    if (SCG_CLKSEL_1 == 3) {
        return GetClock_32k();
    }
    if (SCG_CLKSEL_1 == 0) {
        if (SCG_CLKSEL_0 == 0) {
            /* 12 MHz fast IRC: valid iff SCG status 0x40013010 bit 14 set. */
            return ((SCG_STAT_0x10 & 0x4000u) != 0) ? 12000000u /* 0x00B71B00 */
                                                    : 0u;
        }
        if (SCG_CLKSEL_0 == 1) {
            if ((SCG_STAT_0x20 & 0x01000000u) != 0) {
                return CORE_CLK_FREQ_SEL0_1; /* *(uint32_t *)0x20000010 */
            }
        } else if (SCG_CLKSEL_0 == 2) {
            return Adc_ReadCh_LPO1MHz();
        } else if (SCG_CLKSEL_0 == 3) {
            /* 96 MHz PLL: valid iff SCG status 0x40013010 bit 30 set. */
            return ((SCG_STAT_0x10 & 0x40000000u) != 0) ? 96000000u /* 0x05B8D800 */
                                                        : 0u;
        }
    }
    return 0;
}

/* 0x00007988 */
void periph_clk_nvic_enable(int param_1)
{
    int irqn;

    /* Ungate the peripheral clock (clock-gate controller @ 0x40004000). */
    pcc_gate_set((volatile uint32_t *)0x40004000u, (uint32_t)param_1);

    /* Look up the IRQ number for this gate index (signed byte table). */
    irqn = (int)PERIPH_IRQ_TABLE[param_1];
    if (irqn >= 0) {
        /* Clear any pending state via NVIC ICPR (base 0xE000E100 + 0x180). */
        NVIC_ICPR_WORD(irqn) = 1u << ((uint32_t)irqn & 0x1fu);
    }

    /* Tail-call: enable the interrupt via NVIC ISER helper. */
    nvic_irq_enable((uint32_t)irqn);
}
