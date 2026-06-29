/*
 * clock.c -- VanMoof S5 motor_control (clock + peripheral-clock init).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== peripheral_clocks_init  (OEM 0x081C8F, clock) =====
 * Enables and disables peripheral clocks via CpuSysRegs.PCLKCRx registers (0x5D322-0x5D34C). ORs individual bit-masks to enable clocks for CPUTIMER0-2 (PCLKCR0), EPWM1-8 (PCLKCR2), EQEP1/2 (PCLKCR4), SCIA/B (PCLKCR7), I2CA/B (PCLKCR8), ADC-A/B/C (PCLKCR13), CMPSS1-7 (PCLKCR14-15). Clears (disables) ECAP, CLB and other unused peripherals in the same PCLKCR range. Runs inside EALLOW/EDIS.
 * Peripherals: CpuSysRegs.PCLKCR0 @0x5D322, PCLKCR2 @0x5D326, PCLKCR3 @0x5D328, PCLKCR4 @0x5D32A, PCLKCR6 @0x5D32E, PCLKCR7 @0x5D330, PCLKCR8 @0x5D332, PCLKCR9 @0x5D334, PCLKCR10 @0x5D336, PCLKCR13 @0x5D33C, PCLKCR14 @0x5D33E, PCLKCR15 @0x5D340, PCLKCR16 @0x5D342, PCLKCR18 @0x5D346, PCLKCR19 @0x5D348, PCLKCR20 @0x
 * [confidence: high] */
/* 0x081C8F - enable/disable peripheral clocks in CpuSysRegs.PCLKCRx */
void peripheral_clocks_init(void) {
    EALLOW;

    /* PCLKCR0 @0x5D322: CPUTIMER0|2|3|4|5 */
    CpuSysRegs.PCLKCR0.all |= 1U;
    CpuSysRegs.PCLKCR0.all |= 4U;
    CpuSysRegs.PCLKCR0.all |= 8U;
    CpuSysRegs.PCLKCR0.all |= 16U;
    CpuSysRegs.PCLKCR0.all |= 32U;
    /* clear bit 15 of AH word, set bit 14 of AH word */
    CpuSysRegs.PCLKCR0.all &= ~(1UL << 16);   /* clear bit 16 of 32-bit */
    CpuSysRegs.PCLKCR0.all |=  (4UL << 16);   /* set bit 18 */

    /* PCLKCR2 @0x5D326: EPWM1-8 */
    CpuSysRegs.PCLKCR2.all |= 1U;
    CpuSysRegs.PCLKCR2.all |= 2U;
    CpuSysRegs.PCLKCR2.all |= 4U;
    CpuSysRegs.PCLKCR2.all |= 8U;
    CpuSysRegs.PCLKCR2.all |= 16U;
    CpuSysRegs.PCLKCR2.all |= 32U;
    CpuSysRegs.PCLKCR2.all |= 64U;
    CpuSysRegs.PCLKCR2.all |= 128U;

    /* PCLKCR3 @0x5D328: disable ECAP1-7 */
    CpuSysRegs.PCLKCR3.all &= ~1U;
    CpuSysRegs.PCLKCR3.all &= ~2U;
    CpuSysRegs.PCLKCR3.all &= ~4U;
    CpuSysRegs.PCLKCR3.all &= ~8U;
    CpuSysRegs.PCLKCR3.all &= ~16U;
    CpuSysRegs.PCLKCR3.all &= ~32U;
    CpuSysRegs.PCLKCR3.all &= ~64U;

    /* PCLKCR4 @0x5D32A: EQEP1/2 */
    CpuSysRegs.PCLKCR4.all |= 1U;
    CpuSysRegs.PCLKCR4.all |= 2U;

    /* PCLKCR6 @0x5D32E: disable SPIA (bit0) */
    CpuSysRegs.PCLKCR6.all &= ~1U;

    /* PCLKCR7 @0x5D330: SCIA/B */
    CpuSysRegs.PCLKCR7.all |= 1U;
    CpuSysRegs.PCLKCR7.all |= 2U;

    /* PCLKCR8 @0x5D332: I2CA/B */
    CpuSysRegs.PCLKCR8.all |= 1U;
    CpuSysRegs.PCLKCR8.all |= 2U;

    /* PCLKCR9 @0x5D334: disable CANA (bit0) */
    CpuSysRegs.PCLKCR9.all &= ~1U;

    /* PCLKCR10 @0x5D336 */
    CpuSysRegs.PCLKCR10.all |= 1U;
    CpuSysRegs.PCLKCR10.all |= 2U;

    /* PCLKCR13 @0x5D33C: ADC-A/B/C */
    CpuSysRegs.PCLKCR13.all |= 1U;
    CpuSysRegs.PCLKCR13.all |= 2U;
    CpuSysRegs.PCLKCR13.all |= 4U;

    /* PCLKCR14 @0x5D33E: CMPSS1-7 */
    CpuSysRegs.PCLKCR14.all |= 1U;
    CpuSysRegs.PCLKCR14.all |= 2U;
    CpuSysRegs.PCLKCR14.all |= 4U;
    CpuSysRegs.PCLKCR14.all |= 8U;
    CpuSysRegs.PCLKCR14.all |= 16U;
    CpuSysRegs.PCLKCR14.all |= 32U;
    CpuSysRegs.PCLKCR14.all |= 64U;

    /* PCLKCR15 @0x5D340: PGA or additional CMPSS */
    CpuSysRegs.PCLKCR15.all |= 1U;
    CpuSysRegs.PCLKCR15.all |= 2U;
    CpuSysRegs.PCLKCR15.all |= 4U;
    CpuSysRegs.PCLKCR15.all |= 8U;
    CpuSysRegs.PCLKCR15.all |= 16U;
    CpuSysRegs.PCLKCR15.all |= 32U;
    CpuSysRegs.PCLKCR15.all |= 64U;

    /* PCLKCR16 @0x5D342: DAC-A/B (bits 24/25 = AH bits 8/9) */
    CpuSysRegs.PCLKCR16.all |= (1UL << 16);   /* orb AH,#1 */
    CpuSysRegs.PCLKCR16.all |= (2UL << 16);   /* orb AH,#2 */

    /* PCLKCR18 @0x5D346: disable CLB1/2 */
    CpuSysRegs.PCLKCR18.all &= ~1U;
    CpuSysRegs.PCLKCR18.all &= ~2U;

    /* PCLKCR19 @0x5D348: disable */
    CpuSysRegs.PCLKCR19.all &= ~1U;

    /* PCLKCR20 @0x5D34A: disable */
    CpuSysRegs.PCLKCR20.all &= ~1U;

    EDIS;
}


/* ===== dcc_clock_verify  (OEM 0x081F94, clock) =====
 * Verifies the system oscillator frequency using DCC1 (Dual-Clock Comparator). Selects clock source encoding based on input ACC (0=INTOSC2 reference, 0x10000=INTOSC1, 0x30000=AUXPLL). Enables DCC1 clock via PCLKCR21, programs DCC1CTL mode, DCC1CLKSRC0 (counter 0 = 88), DCC1CLKSRC1, DCC1CNT0SEED and DCC1VALID0SEED (tolerance window based on XAR7 oscillator config bits), DCC1CNT1SEED=24. Polls DCC1STA
 * Peripherals: PCLKCR21 @0x5D34C; DCC1_BASE=0x5E700: DCC1CTL=0x5E700, DCC1COUNTER0=0x5E708, DCC1COUNTER1=0x5E70A, DCC1COUNTER2=0x5E70C, DCC1STATUS=0x5E714, DCC1CNT0SEED=0x5E718, DCC1VALID0SEED=0x5E71C, DCC1CNT1SEED=0x5E720, DCC1CLKSRC1=0x5E724, DCC1CLKSRC0=0x5E728; DCC1CLKSRC0 value=88; DCC1CNT1SEED=24; tolerance 
 * [confidence: medium] */
/* 0x081F94 - verify oscillator via DCC1; returns 1 if clock matches, 0 if not */
/* ACC = oscillator type: 0=ref, 0x10000=INTOSC1, 0x30000=AUXPLL */
/* *-SP[arg_2] = ClkSrcCtl config word; XAR7 = ptr to clock config struct */
uint16_t dcc_clock_verify(uint32_t osc_sel, ClkCfgWord *clk_cfg) {
    uint16_t pl;  /* clock source index 0..2 */

    if (osc_sel == 0) {
        pl = 2;
    } else if (osc_sel == 0x10000UL || osc_sel == 0x30000UL) {
        pl = 0;
    } else {
        pl = 1;
    }

    /* Decode clock config: bits[6:0] = base count, bits[14:13] = tolerance tier */
    uint16_t base_cnt = (uint16_t)(clk_cfg->cfg & 0x7F);
    uint16_t tier     = (uint16_t)((clk_cfg->cfg & 0x6000U) >> 13);
    uint32_t cnt0_seed = (uint32_t)base_cnt * 100UL; /* /* TODO verify multiplier */ */

    /* Tolerance offset by tier */
    if      (tier == 1) cnt0_seed += 25;
    else if (tier == 2) cnt0_seed += 50;
    else if (tier == 3) cnt0_seed += 75;

    /* Enable DCC1 clock (PCLKCR21 bit0) */
    EALLOW;
    CpuSysRegs.PCLKCR21.all |= 1U;  /* 0x5D34C */
    EDIS;

    /* Configure DCC1 */
    EALLOW;
    DCC1Regs.DCCCLKSRC1.all |= 1U;  /* 0x5E724 */
    DCC1Regs.DCCCLKSRC1.all |= 2U;
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0xFFF0) | 5U;     /* mode bits */
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0xFF1F) | 0x80U;  /* enable */
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0x0FFF) | 0x5000U;
    DCC1Regs.DCCCLKSRC0.all = (DCC1Regs.DCCCLKSRC0.all & 0xFFF0) | pl; /* src0 = osc index */
    DCC1Regs.DCCCLKSRC1.all = (DCC1Regs.DCCCLKSRC1.all & 0xFF0F) | 0xA000U; /* TODO verify */
    /* Counter seeds */
    *(uint16_t*)0x5E710 = 88;    /* DCC1CLKSRC0 value */
    *(uint16_t*)0x5E720 = 24;    /* DCC1CNT1SEED */
    *(uint16_t*)0x5E712 = (uint16_t)cnt0_seed;         /* DCC1CNT0SEED low */
    *(uint16_t*)0x5E728 = (uint16_t)(cnt0_seed >> 16); /* DCC1CNT0SEED high / valid seed */
    /* MODE / DONE/ERROR select bits */
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0xF8FF) | 0x0A00U;
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0xFF8F) | 0x00A0U;
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0x0FFF) | 0xA000U;
    DCC1Regs.DCCCTL.all  = (DCC1Regs.DCCCTL.all & 0xFFF8) | 10U;
    EDIS;

    /* Poll DCC1STATUS until done (bits[1:0] != 0) or timeout */
    uint32_t timeout = cnt0_seed;  /* reuse seed as timeout */
    while ((DCC1Regs.DCCSTS.all & 0x3U) == 0) {
        if (--timeout == 0) break;
    }
    if ((DCC1Regs.DCCSTS.all & 0x3U) == 0) return 0; /* timeout = mismatch */

    /* Check STATUS: DONE=bit1, ERROR=bit0 -- DONE without ERROR = OK */
    if ((*(uint16_t*)0x5E71C & 0x3U) != 2U) return 0;
    if (*(uint16_t*)0x5E718 != 0)            return 0;
    if (*(uint16_t*)0x5E71C != 0)            return 0;
    uint16_t ok = (*(uint16_t*)0x5E720 == 0) ? 1U : 0U;
    return ok;
}


/* ===== sysclk_pll_init  (OEM 0x0820EC, clock) =====
 * Configures the system PLL and clock source. Checks ClkCfgRegs.X1CNT (0x5D22E) bit 0; if already set (crystal oscillator already counted), returns 0 immediately. Otherwise: calls sub_82C32 to select the oscillator source, disables SYSCLKDIVSEL (0x5D20E bit1), waits 11 cycles, extracts clock fields from input argument, programs LOSPCP (0x5D214) divider, enables XTAL (0x5D216), iterates up to 99 time
 * Peripherals: ClkCfgRegs.X1CNT @0x5D22E; SYSCLKDIVSEL @0x5D20E; LOSPCP @0x5D214; XTALCR @0x5D216; CLKFAILCFG @0x5D222; delay unk_E65A; sub_82C32 (osc source select); sub_81F94 (DCC verify); retry counter 99 (XAR2=99); timeout 2000 words for XTAL ready
 * [confidence: medium] */
/* 0x0820EC - init system PLL and switch to XTAL/PLL; returns 1 OK, 0 fail */
uint16_t sysclk_pll_init(uint32_t clk_cfg_word) {
    uint32_t saved_cfg = clk_cfg_word;

    /* Skip if crystal already stable (X1CNT bit 0 set) */
    if (ClkCfgRegs.X1CNT.all & 1U) {  /* 0x5D22E */
        return 0;
    }

    /* Select oscillator source, get osc handle */
    uint32_t osc_sel = clk_cfg_word & 0x0003UL;  /* bits[1:0] */
    uint32_t *osc    = osc_source_select(osc_sel); /* sub_82C32 */

    /* Disable SYSCLKDIVSEL bit1 (halve sysclk during transition) */
    EALLOW;
    ClkCfgRegs.SYSCLKDIVSEL.all &= ~2U;  /* 0x5D20E */
    EDIS;

    delay_cycles(11);  /* unk_E65A(11) */

    /* Extract LOSPCP divider from config word bits[10:9] >> 5 */
    uint16_t lospcp_div = (uint16_t)((saved_cfg >> 5) & 0x300U);
    /* Extract XTALCR bits [6:0] of saved_cfg */
    uint16_t xtal_ctl   = (uint16_t)(saved_cfg & 0x607FUL);

    EALLOW;
    ClkCfgRegs.LOSPCP.all   = lospcp_div;  /* 0x5D214 -- low-speed clock divider */
    ClkCfgRegs.XTALCR.all  &= ~1U;         /* 0x5D216 -- OSCOFF=0, enable crystal */
    EDIS;

    /* Wait for XTAL oscillation (poll XTALCR.XTALCR bit0; timeout=2000) */
    uint16_t xtal_ready = 0;
    {
        uint32_t timeout = 2000;
        EALLOW;
        while (timeout-- && !(ClkCfgRegs.XTALCR.all & 1U)); /* 0x5D216 */
        EDIS;
    }

    /* Try up to 99 times to get DCC confirmation */
    uint16_t dcc_ok = 0;
    for (int retry = 99; retry >= 0; retry--) {
        dcc_ok = dcc_clock_verify((uint32_t)osc, clk_cfg_word); /* sub_81F94 */
        if (dcc_ok != 0) break;
        if (retry == 0) return 0; /* exhausted retries */
    }
    if (dcc_ok == 1) return 1; /* already good, fall through */

    /* Program CLKFAILCFG (0x5D222) with osc-index bits */
    uint16_t osc_idx = (uint16_t)(xtal_ctl >> 7) & 0x3F;
    EALLOW;
    if (osc_idx == 63) {
        ClkCfgRegs.CLKFAILCFG.all = ClkCfgRegs.CLKFAILCFG.all | 63U; /* orb #63 */
    } else {
        uint16_t tmp = (uint16_t)(ClkCfgRegs.CLKFAILCFG.all & 0xFFC0) | (1U + osc_idx);
        ClkCfgRegs.CLKFAILCFG.all = tmp;
    }
    /* Re-enable SYSCLKDIVSEL bit1 */
    ClkCfgRegs.SYSCLKDIVSEL.all |= 2U;  /* 0x5D20E */
    EDIS;

    delay_cycles(40);

    /* Restore CLKFAILCFG keeping upper bits, insert osc_idx */
    EALLOW;
    ClkCfgRegs.CLKFAILCFG.all = (ClkCfgRegs.CLKFAILCFG.all & 0xFFC0) | osc_idx;
    EDIS;

    return 1;
}


/* ===== pll_switch_to_xtal_and_set_fmult  (OEM 0x082D0A, clock) =====
 * Switches the system oscillator source to the XTAL input (clears CLKSRCCTL1 bits [1:0]), waits for the clock to settle via sub_82E3E, then sets SYSPLLMULT fractional multiplier to 1 (bits [1:0] := 01). Finally polls SYSPLLSTS bit 0 in a spin-wait until the PLL re-locks. 1 caller.
 * Peripherals: ClkCfg block (EALLOW-protected): 0x5D232 = CLKSRCCTL1 (oscillator source select, clear bits [1:0] to select XTAL); 0x5D208 = SYSPLLMULT (mask 0xFFFC then set bit 0 = FMULT fractional=1); 0x5D22E = SYSPLLSTS (poll bit 0 for PLL lock). Calls sub_82E3E (crystal settle delay).
 * [confidence: medium] */
/* @0x082D0A - PLL reconfiguration: switch to XTAL, update FMULT, wait for lock */
extern void xtal_settle_wait(void); /* sub_82E3E */

void pll_switch_to_xtal_and_set_fmult(void)
{
    volatile uint16_t *CLKSRCCTL1 = (volatile uint16_t *)0x5D232u;
    volatile uint16_t *SYSPLLMULT = (volatile uint16_t *)0x5D208u;
    volatile uint16_t *SYSPLLSTS  = (volatile uint16_t *)0x5D22Eu;
    uint16_t tmp;

    /* Select XTAL as oscillator source: clear bits [1:0] of CLKSRCCTL1 */
    EALLOW;
    *CLKSRCCTL1 &= 0xFFFEu;  /* clear bit 0 (OSCCLKSRCSEL[0]) */
    *CLKSRCCTL1 &= 0xFFFDu;  /* clear bit 1 (OSCCLKSRCSEL[1]) */
    EDIS;

    xtal_settle_wait(); /* wait for oscillator + PLL to stabilise */

    /* Set SYSPLLMULT fractional multiplier field to 1 */
    EALLOW;
    tmp = *SYSPLLMULT & 0xFFFCu; /* clear FMULT[1:0] */
    tmp |= 1u;                    /* FMULT = 01b */
    *SYSPLLMULT = tmp;
    EDIS;

    /* Poll SYSPLLSTS bit 0 until PLL locks */
    while (*SYSPLLSTS & 0x0001u)
    {
        /* PLL not yet locked - loop back and re-apply FMULT if needed */
        EALLOW;
        *SYSPLLMULT |= 2u;        /* set bit 1 (FMULT[1]) - retry */
        EDIS;
        xtal_settle_wait();
        EALLOW;
        tmp = *SYSPLLMULT & 0xFFFCu;
        tmp |= 1u;
        *SYSPLLMULT = tmp;
        EDIS;
    }
}


/* ===== xtal_settle_and_x1cnt_check  (OEM 0x082E3E, clock) =====
 * Crystal oscillator settle sequence: calls ROM delay (unk_E65A) with count 2000, then loops 3 times verifying the X1CNT crystal counter register (at 0x5D230). If X1CNT < 511 (0x1FF), sets bit 8 in the register and waits until X1CNT >= 511. Then waits until X1CNT equals 1023 (0x3FF, full crystal cycle count), confirming XTAL is stable. 3 callers.
 * Peripherals: ClkCfg: 0x5D230 = X1CNT (10-bit crystal oscillator cycle counter); 0x5D228 = XTALCR or adjacent oscillator control register (bit 8 = XTALEN or counter reset). ROM delay function unk_E65A. Constants: 2000 (ROM delay count), 511 (0x1FF lower threshold), 1023 (0x3FF = fully counted).
 * [confidence: medium] */
/* @0x082E3E - Crystal oscillator settle: call ROM delay, verify X1CNT reaches 1023
 * Called after oscillator source switch to confirm XTAL stable.
 */
extern void rom_delay(uint32_t count);  /* unk_E65A (ROM/library) */

void xtal_settle_and_x1cnt_check(void)
{
    volatile uint16_t *X1CNT = (volatile uint16_t *)0x5D230u;
    int loop;

    for (loop = 3; loop > 0; loop--)
    {
        rom_delay(2000u); /* ROM delay: wait 2000 reference cycles */

        /* If X1CNT < 511: kick the oscillator count enable (set bit 8) */
        if (*X1CNT < 511u)
        {
            do {
                /* Set bit 8 of X1CNT control register to re-enable counting TODO verify */
                volatile uint16_t *XTALCR = (volatile uint16_t *)0x5D230u;
                *XTALCR |= (1u << 8); /* set bit 8 */
            } while (*X1CNT < 511u);
        }

        /* Wait until X1CNT reaches 1023 (fully counted = XTAL stable) */
        while (*X1CNT != 1023u)
        {
            /* poll */
        }
    } /* repeat 3 times for confidence */
}


/* ===== clock_config_xtal_pll_div  (OEM 0x082F95, clock) =====
 * Configures the crystal oscillator and PLL clock divider. EALLOW-protected: clears bit0 and sets bit1 of XTALCR2/CLKFAILCFG (0x5D232), then calls sub_82E3E (oscillator settling wait loop). Then sets SYSCLKDIVSEL (0x5D208) to divide-by-2 (AND 0xFFFC | 1). Finally spin-polls bit0 of MCDCR (0x5D22E) waiting for clock stabilisation, then clears product-shift mode (spm 0).
 * Peripherals: ClkCfgRegs (base 0x5D200): XTALCR2/CLKFAILCFG @ 0x5D232 bit1 set/bit0 clr; SYSCLKDIVSEL @ 0x5D208 bits1:0=0b01; MCDCR @ 0x5D22E bit0 (poll). EALLOW/EDIS required.
 * [confidence: medium] */
/* 0x082F95 - clock_config_xtal_pll_div */
extern void sub_82E3E(void); /* oscillator/PLL calibration wait */
void clock_config_xtal_pll_div(void)
{
    volatile uint16_t *xtalcr2     = (volatile uint16_t *)0x5D232uL;
    volatile uint16_t *sysclkdivsel = (volatile uint16_t *)0x5D208uL;
    volatile uint16_t *mcdcr        = (volatile uint16_t *)0x5D22EuL;

    EALLOW;
    *xtalcr2 &= ~0x0001u; /* clear bit0 */
    *xtalcr2 |=  0x0002u; /* set   bit1 */
    EDIS;

    sub_82E3E(); /* wait for oscillator to settle */

    EALLOW;
    uint16_t div = *sysclkdivsel & 0xFFFCu;
    div |= 0x0001u;        /* divide-by-2 */
    *sysclkdivsel = div;
    EDIS;

    /* estop0 debug breakpoint then spin until clock locked */
    while (*mcdcr & 0x0001u)
        ; /* wait for MCDCR bit0 to clear (clock OK) */

    __asm(" spm 0"); /* reset product shift mode */
}
