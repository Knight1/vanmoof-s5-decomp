/*
 * sysclock.c - clock / PLL / flash bring-up.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Faithful translation of SystemClock_PllFlashInit @ 0x00001244. Every
 * register write/poll, magic constant and trim read is reproduced verbatim.
 * Frequencies are S32K-style (96 MHz core / SPLL, 16 MHz FIRC, 12 MHz SIRC).
 */

#include <stdint.h>

#include "clock.h"   /* Adc_ReadCh_LPO1MHz, clock_div_program */
#include "util.h"    /* busy_wait */
#include "sysclock.h"

/* ---- VanMoof helpers not yet translated (exact Ghidra names) ---- */

/* Resolve the 32 kHz clock frequency, in Hz. // 0x00001188 */
extern uint32_t GetClock_32k(void);

/* ---- Absolute MMIO / data-block base addresses (from the literal pool) ---- */

#define SCG_BASE      0x40000000u  /* System Clock Generator             */
#define FTFC0_BASE    0x40020000u  /* FTFC flash controller (#1)         */
#define FTFC1_BASE    0x40034000u  /* FTFC flash controller (#2)         */
#define CTRL_BASE     0x40013000u  /* control block (SCG cfg shadow)     */

/* RAM frequency publication targets. */
#define SYSCLK_RAM_A  0x20000010u  /* // DAT_00001554 */
#define SYSCLK_RAM_B  0x20001654u  /* // DAT_00001584 (SystemCoreClock)  */
#define FREQ_RAM_C    0x20000000u  /* // DAT_00001590 */

/* Frequency / threshold constants (verbatim from the literal pool). */
#define FREQ_16MHZ    0x00f42400u  /* 16000000  // DAT_00001550 */
#define SOSC_NOM      0x01267ea0u  /* 19300000  // DAT_00001558 */
#define SOSC_THR_LO   0x0101dfa0u  /* 16899999  // DAT_0000155c */
#define SOSC_THR_HI   0x014b1da0u  /* 21700000  // DAT_0000157c */
#define SOSC_CR_MASK  0xffff83e0u  /* // DAT_00001560 */
#define FREQ_96MHZ    0x05b8d800u  /* 96000000  // DAT_00001568 */
#define FREQ_24576K   0x01770000u  /* 24576000  // DAT_00001588 */
#define FREQ_12MHZ    0x00b71b00u  /* 12000000  // DAT_00001594 */
#define PLL_TOL       0x012fa660u  /* 19900000  // DAT_00001580 */
#define DELAY_150K    0x000249f0u  /* 150000    // DAT_00001598 */
#define PLL_SRC_MASK  0x03fffc00u  /* // DAT_00001578 */
#define SPLLCFG_VAL   0x100c0000u  /* // DAT_00001574 */
#define PCC_PRE_FIRC  0x00207ca0u  /* // DAT_00001570 */
#define PCC_PRE_INIT  0x00109108u  /* // DAT_0000154c */
#define PCC_LATE      0x00109408u  /* // DAT_0000158c */

/* IFR factory-trim addresses (on-chip flash info block; not in this image). */
#define IFR_FIRC_TRIM 0x0003fce8u  /* // _DAT_0003fce8 (+0:flag, +4:val)  */
#define IFR_SIRC_T1   0x0003fd30u  /* // _DAT_0003fd30 */
#define IFR_SIRC_T2   0x0003fd40u  /* // _DAT_0003fd40 */

#define MMIO32(a)     (*(volatile uint32_t *)(uintptr_t)(a))

/*
 * Flash read-clock divider lookup table @ 0x0000a4a0 (8 entries x 8 bytes).
 * Each entry is { value, threshold }: scan for the first entry whose threshold
 * is >= 96 MHz (unsigned) and take (value & 0xf) as the flash clock divider;
 * if none match, the divider defaults to 0xf. Reproduced VERBATIM.
 */
static const uint32_t pll_flash_clk_table[16] = {
    0x20000021u, 0x48003800u,  /* // DAT @ 0xa4a0 */
    0x48004800u, 0x48004800u,
    0x48004800u, 0x18003000u,
    0x38002000u, 0x48004800u,
    0x48004800u, 0x48004800u,
    0x30004800u, 0x00210018u,
    0x30001800u, 0x48004800u,
    0x48004800u, 0x10002800u,
};

void SystemClock_PllFlashInit(void)  /* // 0x00001244 */
{
    uint32_t sosc_min;
    uint32_t sosc_alt;
    uint32_t flash_div;
    int i;

    /* --- FTFC flash housekeeping + SCG initial bring-up --- */
    MMIO32(FTFC0_BASE + 0xc8) = 0x20;          /* // r8 = 0x20 */
    MMIO32(SCG_BASE + 0x148) = 0x8000000;      /* // r7 = 0x8000000 */
    MMIO32(SCG_BASE + 0x228) = 0x8000000;
    MMIO32(FTFC0_BASE + 0xc8) = 0x20;

    /* SCG cfg shadow (0x40013000 +0x10) |= 0x4000, then program PCC. */
    MMIO32(CTRL_BASE + 0x10) |= 0x4000;
    clock_div_program(PCC_PRE_INIT);                /* // 0x00109108 */

    MMIO32(SCG_BASE + 0xa18) |= 0x40;
    MMIO32(SCG_BASE + 0x148) = 0x8000000;
    MMIO32(SCG_BASE + 0x228) = 0x8000000;
    MMIO32(FTFC0_BASE + 0xc8) = 0x20;

    MMIO32(CTRL_BASE + 0x10) |= 0x40000000;
    MMIO32(FTFC0_BASE + 0xc8) = 0x100;
    MMIO32(FTFC0_BASE + 0xc8) = 0x100000;
    MMIO32(FTFC0_BASE + 0xc8) = 0x100;
    MMIO32(FTFC0_BASE + 0xc8) = 0x100000;

    MMIO32(SCG_BASE + 0xa18) |= 0x20;
    MMIO32(SYSCLK_RAM_A) = FREQ_16MHZ;         /* // *DAT_00001554 = 16 MHz */
    MMIO32(SCG_BASE + 0xa18) |= 0x20;
    MMIO32(CTRL_BASE + 0x20) |= 0x1000000;

    /* --- Apply factory FIRC trim from flash IFR (if flagged valid) --- */
    if ((int32_t)(MMIO32(IFR_FIRC_TRIM) << 0x1f) < 0) {
        MMIO32(FTFC0_BASE + 0x10) = MMIO32(IFR_FIRC_TRIM) >> 1;
        MMIO32(FTFC0_BASE + 0x14) = MMIO32(IFR_FIRC_TRIM + 4);
    }

    /* --- Resolve the system-oscillator frequency from two trim words --- */
    sosc_min = SOSC_NOM;                        /* // DAT_00001558 = 19300000 */
    if ((int32_t)(MMIO32(IFR_SIRC_T1) << 0x1f) < 0) {
        sosc_min = MMIO32(IFR_SIRC_T1) >> 1;
    }
    sosc_alt = SOSC_NOM;
    if ((int32_t)(MMIO32(IFR_SIRC_T2) << 0x1f) < 0) {
        sosc_alt = MMIO32(IFR_SIRC_T2) >> 1;
    }
    if (sosc_alt <= sosc_min) {
        sosc_min = sosc_alt;
    }

    /* --- Program SOSC range (CR @ +0x1c) and divider field (@ +0x10) --- */
    if (SOSC_THR_LO < sosc_min) {               /* // DAT_0000155c = 16899999 */
        uint32_t cr = MMIO32(FTFC0_BASE + 0x1c) & SOSC_CR_MASK;
        if (SOSC_THR_HI < sosc_min) {           /* // DAT_0000157c = 21700000 */
            MMIO32(FTFC0_BASE + 0x1c) = cr | 0x5811;
            MMIO32(FTFC0_BASE + 0x10) =
                (MMIO32(FTFC0_BASE + 0x10) & 0xffe1ffff) | 0x60000;
        } else {
            MMIO32(FTFC0_BASE + 0x1c) = cr | 0x6c16;
            MMIO32(FTFC0_BASE + 0x10) =
                (MMIO32(FTFC0_BASE + 0x10) & 0xffe1ffff) | 0xa0000;
        }
    } else {
        MMIO32(FTFC0_BASE + 0x1c) =
            (MMIO32(FTFC0_BASE + 0x1c) & SOSC_CR_MASK) | 0x7c1e;
        MMIO32(FTFC0_BASE + 0x10) =
            (MMIO32(FTFC0_BASE + 0x10) & 0xffe1ffff) | 0x100000;
    }

    /* --- Pick the flash read clock divider from the OEM lookup table --- */
    flash_div = 0xf;
    for (i = 0; i != 8; i++) {
        if (pll_flash_clk_table[i * 2 + 1] >= FREQ_96MHZ) {
            flash_div = pll_flash_clk_table[i * 2];
            break;
        }
    }

    /* --- Program FTFC FOPT / flash clock + wait-states --- */
    MMIO32(FTFC1_BASE + 0xfe8) = 0x1f;
    MMIO32(FTFC1_BASE + 0x80) =
        (MMIO32(FTFC1_BASE + 0x80) & 0xfffffff0) | (flash_div & 0xf);
    MMIO32(FTFC1_BASE + 0x0) = 2;
    while (-1 < (int32_t)(MMIO32(FTFC1_BASE + 0xfe0) << 0x1d)) {
        /* spin until FTFC reports ready */
    }
    MMIO32(SCG_BASE + 0x400) =
        ((flash_div & 0xf) << 0xc) | (MMIO32(SCG_BASE + 0x400) & 0xffff0fff);

    clock_div_program(0x20c);

    /* --- SPLL bring-up (0x40000580 block) for 96 MHz --- */
    MMIO32(SCG_BASE + 0x580) = PCC_PRE_FIRC;    /* // DAT_00001570 */
    MMIO32(FTFC0_BASE + 0xc8) = 0x200;
    MMIO32(FTFC0_BASE + 0xc8) = 0x800000;
    MMIO32(FTFC0_BASE + 0xc0) = 0x200;
    MMIO32(FTFC0_BASE + 0xc0) = 0x800000;

    MMIO32(SCG_BASE + 0x588) = 0x19;
    MMIO32(SCG_BASE + 0x588) = 0x119;
    MMIO32(SCG_BASE + 0x58c) = 0xa;
    MMIO32(SCG_BASE + 0x58c) = 0x2a;
    MMIO32(SCG_BASE + 0x590) = 0;
    MMIO32(SCG_BASE + 0x594) = SPLLCFG_VAL;     /* // DAT_00001574 = 0x100c0000 */
    MMIO32(SCG_BASE + 0x594) = (SPLLCFG_VAL + 0x4000000) + 2;

    MMIO32(FTFC0_BASE + 0xc8) = 0x200;
    MMIO32(FTFC0_BASE + 0xc8) = 0x800000;

    /* --- Verify the PLL output frequency is within tolerance --- */
    if ((PLL_SRC_MASK & MMIO32(SCG_BASE + 0x594)) != 0) {
        uint32_t src_freq;
        uint32_t divider;

        switch (MMIO32(SCG_BASE + 0x290) & 7) {
        case 0:
            src_freq = FREQ_12MHZ;              /* // DAT_00001594 */
            break;
        case 1:
            src_freq = 0;
            if ((MMIO32(CTRL_BASE + 0x20) & 0x1000000) != 0) {
                src_freq = FREQ_16MHZ;          /* // DAT_00001550 */
            }
            break;
        case 2:
            src_freq = Adc_ReadCh_LPO1MHz();
            break;
        case 3:
            src_freq = GetClock_32k();
            break;
        default:
            src_freq = 0;
            break;
        }

        if ((int32_t)(MMIO32(SCG_BASE + 0x580) << 0xc) < 0) {
            divider = 1;
        } else {
            divider = MMIO32(SCG_BASE + 0x588) & 0xff;
            if (divider == 0) {
                divider = 1;
            }
        }

        if ((src_freq / divider) - 100000 <= PLL_TOL) {  /* // 0x18600 + 0xa0 */
            while ((int32_t)(MMIO32(SCG_BASE + 0x584) << 0x1f) >= 0) {
                /* spin until PLL valid */
            }
            goto pll_locked;
        }
    }

    busy_wait(DELAY_150K);                   /* // DAT_00001598 = 150000 */

pll_locked:
    /* --- Publish SystemCoreClock and program all SCG clock dividers --- */
    MMIO32(SYSCLK_RAM_B) = FREQ_24576K;         /* // *DAT_00001584 = 24576000 */

    MMIO32(SCG_BASE + 0x380) = 0;
    MMIO32(SCG_BASE + 0x300) = 0x20000000;
    MMIO32(SCG_BASE + 0x300) = SCG_BASE;        /* // r3 = 0x40000000 */
    MMIO32(SCG_BASE + 0x300) = 0;
    MMIO32(SCG_BASE + 0x324) = 0xff;
    MMIO32(SCG_BASE + 0x32c) = 0xff;
    MMIO32(SCG_BASE + 0x330) = 0xff;

    MMIO32(SCG_BASE + 0x388) = 0x20000000;
    MMIO32(SCG_BASE + 0x388) = SCG_BASE;
    MMIO32(SCG_BASE + 0x388) = 1;
    MMIO32(SCG_BASE + 0x38c) = 0x20000000;
    MMIO32(SCG_BASE + 0x38c) = SCG_BASE;
    MMIO32(SCG_BASE + 0x38c) = 0;
    MMIO32(SCG_BASE + 0x3c4) = 0x20000000;
    MMIO32(SCG_BASE + 0x3c4) = SCG_BASE;
    MMIO32(SCG_BASE + 0x3c4) = 0;
    MMIO32(SCG_BASE + 0x30c) = 0x20000000;
    MMIO32(SCG_BASE + 0x30c) = SCG_BASE;
    MMIO32(SCG_BASE + 0x30c) = 3;

    clock_div_program(PCC_LATE);                     /* // 0x00109408 */
    clock_div_program(0x315);
    clock_div_program(0x217);
    clock_div_program(0x418);
    clock_div_program(0x41c);
    clock_div_program(0x100);
    clock_div_program(0x110);

    MMIO32(FREQ_RAM_C) = FREQ_96MHZ;            /* // *DAT_00001590 = 96000000 */
}
