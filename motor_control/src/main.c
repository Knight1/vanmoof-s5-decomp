/*
 * main.c -- VanMoof S5 motor_control (startup / C-runtime / main).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== codestart_entry  (OEM 0x080000, startup) =====
 * Entry point: single instruction that jumps to codestart_0 (C-runtime _c_int00). This is the reset vector target placed at the top of FLASH.
 * Peripherals: FLASH @0x080000 (word address); branches to codestart_0
 * [confidence: high] */
/* 0x080000 - reset vector / codestart stub */
void codestart_entry(void) {
    /* lb codestart_0 */
    codestart_0();
}


/* ===== periph_handle_table_init  (OEM 0x0824AE, startup) =====
 * Fills a large 'peripheral handle' struct (minimum size 72 words checked at entry) with base addresses for every peripheral used by the motor controller: ADC-A/B/C config registers, ADC result registers, EPWM1-3, CPUTIMERs 0-2, SCIA, SPIA/B, CMPSS bases, CLA/CLB blocks, and more. Stores these base addresses as 32-bit pointers at fixed word-offsets into the struct. Enables a GPIO output bit at 0x702
 * Peripherals: Struct fields: [0]=ADCA@0x7400, [2]=ADCB@0x7480, [4]=ADCC@0x7500, [6]=ADCRESULTA@0xB00, [8]=ADCRESULTB@0xB20, [10]=ADCRESULTC@0xB40, [14]=EPWM1@0x4000, [16]=EPWM2@0x4100, [18]=EPWM3@0x4200, [20]=CPUTIMER0@0xC00, [22]=CPUTIMER1@0xC08, [24]=CPUTIMER2@0xC10, [26]=SCIA@0x7200, [28]=SPIA@0x6100, [30]=SPI
 * [confidence: medium] */
/* 0x0824AE - initialise peripheral handle struct; returns pointer to filled struct */
/* XAR4 = size-hint, ACC = struct pointer (same as XAR6) */
PeriphHandles *periph_handle_table_init(uint32_t acc_size_hint, PeriphHandles *h) {
    if (acc_size_hint < 72U) {
        return NULL;  /* struct too small */
    }

    /* Enable GPIO output pin at 0x7029 (bits 3,5,6) */
    EALLOW;
    *(volatile uint16_t *)0x7029 = (*(volatile uint16_t *)0x7029) | 0x68U;
    EDIS;

    /* ADC config register bases */
    h->adcA_cfg    = (void *)0xB20;   /* XAR0=8:  ADCRESULTB */ /* TODO check slot 8 */
    h->adcB_cfg    = (void *)0xB40;   /* XAR0=10: ADCRESULTC */
    h->sci_a       = (void *)0x7200;  /* XAR0=26: SCIA */
    h->spi_a       = (void *)0x6100;  /* XAR0=28: SPIA */
    h->spi_b       = (void *)0x6110;  /* XAR0=30: SPIB */
    h->cmpss5      = (void *)0x5CA0;  /* XAR0=32 */
    h->cmpss6      = (void *)0x5D00;  /* XAR0=34 */
    h->cmpss7      = (void *)0x5D40;  /* XAR0=36 */
    h->cmpss2      = (void *)0x5C10;  /* XAR0=38 */
    h->cmpss1      = (void *)0x5C00;  /* XAR0=40 */
    h->clb1        = (void *)0x1000;  /* XAR0=42 */
    h->clb2        = (void *)0x1020;  /* XAR0=44 */
    h->clb3        = (void *)0x1040;  /* XAR0=46 */
    h->clb4        = (void *)0x1060;  /* XAR0=48 */
    h->clb5        = (void *)0x1080;  /* XAR0=50 */
    h->epwm1       = (void *)0x4000;  /* XAR0=14 */
    h->epwm2       = (void *)0x4100;  /* XAR0=16 */
    h->epwm3       = (void *)0x4200;  /* XAR0=18 */
    h->timer0      = (void *)0x0C00;  /* XAR0=20 */
    h->timer1      = (void *)0x0C08;  /* XAR1=22 */
    h->timer2      = (void *)0x0C10;  /* XAR0=24 */
    h->adcresult_a = (void *)0x07400; /* *XAR2[0] = ADCA config */ /* TODO verify row 0 assignment */
    h->adcresult_b = (void *)0x07480; /* *XAR2[2] = ADCB */
    h->adcresult_c = (void *)0x07500; /* *XAR2[4] = ADCC */
    h->adc_result_buf = (void *)0x0B00; /* *XAR2[6] = ADCRESULTA */
    h->timer1_cmpss  = (void *)0x0C08;  /* XAR6=3080 */
    h->timer2_cmpss  = (void *)0x0C10;  /* XAR5=3088 */
    h->adcresult_c2  = (void *)(0x0B40 + 64U); /* XAR0=62 */

    periph_handle_finalize(h);  /* sub_831C7(XAR4 = h+64) */

    return h;
}


/* ===== app_main_trampoline  (OEM 0x083143, startup) =====
 * Boot trampoline that unconditionally calls the application main-code entry point (loc_8065C, the second flash region loaded at 0x8065C). Always passes AL=0, XAR4=0. Called from the C-runtime startup sequence after memory/data init. The dead-code table-read path (loc_8314A) is unreachable because movl @ACC,XAR4 always zeros ACC.
 * Peripherals: loc_8065C = application main code region entry point (0x8065C)
 * [confidence: high] */
/* @0x083143 - app_main_trampoline */
extern void app_main(uint16_t arg_al, uint16_t *arg_xar4); /* loc_8065C */

void app_main_trampoline(void) {
    uint16_t *xar4 = NULL;  /* movl XAR4,#0 */
    /* movl @ACC,XAR4 => ACC=0, NEQ branch never taken */
    /* dead path: AL=*xar4; xar4+=2 */
    uint16_t al = 0; /* movb AL,#0 */
    app_main(al, xar4); /* lcr loc_8065C */
}


/* ===== codestart_0  (OEM 0x083173, startup) =====
 * C28x reset vector entry point (the 'codestart' boot stub). With EALLOW active: writes 0x68 to WDCR (0x7029) to disable the watchdog (WDDIS=1, WDCHK=101b). Then jumps to loc_83060 which initialises the stack pointer (SP=246), clears the product-mode shift (spm 0), and sets C28x object/address/map mode bits (c28obj/c28addr/c28map). Flow continues into C-runtime data-copy and sub_831DA init.
 * Peripherals: WDCR @ 0x7029 (SYSCTRL peripheral, offset 0x29); value 0x68 = WDDIS|WDCHK; SP initialised to 246 (0xF6, within M0 RAM)
 * [confidence: high] */
/* @0x083173 - codestart_0 (reset entry, lb from 0x080000) */
#define WDCR  (*(volatile uint16_t *)0x7029u)
#define WDCR_DISABLE  0x68u  /* WDDIS=1, WDCHK=101b */

void codestart_0(void) {
    EALLOW;
    WDCR = WDCR_DISABLE; /* disable watchdog */
    EDIS;
    /* loc_83060: C-runtime pre-init */
    SP = 246;   /* initialise stack pointer */
    SPM(0);     /* product shift = 0 */
    /* c28obj / c28addr / c28map mode bits set (CPU mode registers) */
    /* ... C-runtime data init (sub_831DA), then app_main_trampoline ... */
}


/* ===== cpu_mode_init  (OEM 0x0831CD, startup) =====
 * Initialises C28x CPU operating mode: clears the DBGM bit in ST1 (allowing the debug interface to halt the CPU) and sets the product-shift-mode register SPM to 0 (no arithmetic right-shift of the multiplier product, i.e. 32-bit integer math mode). Called once during the main peripheral setup sequence (0x80961) after interrupts are briefly re-enabled.
 * Peripherals: ST1 register (DBGM bit); SPM field (product shift mode); no MMIO
 * [confidence: high] */
/* 0x0831CD - cpu_mode_init */
/* Clears DBGM (enable CPU halt on debug event) and sets SPM=0 (no product shift). */
void cpu_mode_init(void)
{
    /* clrc DBGM - clear Debug-halt Mode bit in ST1 */
    __asm(" clrc DBGM");
    /* spm 0 - product shift mode = 0 (full 32-bit product, no shift) */
    __asm(" spm 0");
}


/* ===== init_check_always_ok  (OEM 0x0831DA, startup) =====
 * Returns 1 (true) unconditionally. Called at 0x8306D inside the C-runtime startup sequence (codestart_0 fragment at 0x83060) before conditionally invoking sub_82C8B (the registered-hook dispatcher). Acts as a static 'hardware present / init succeeded' check that always succeeds in this build, gating the execution of registered init callbacks.
 * Peripherals: None; returns AL=1 in accumulator
 * [confidence: high] */
/* 0x0831DA - init_check_always_ok */
/* Always returns 1 (init check unconditionally passes in this build). */
uint16_t init_check_always_ok(void)
{
    return 1;
}
