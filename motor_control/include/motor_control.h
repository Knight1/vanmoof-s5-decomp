#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

/*
 * motor_control.h -- shared declarations for the VanMoof S5 BLDC motor-drive
 * controller reference reconstruction.
 *
 * Target: TI TMS320F280049C (C2000, C28x core). Hand-translated from the IDA
 * tms32028 disassembly (build/ida/functions.asm). This is REFERENCE C, not
 * compiled -- there is no C28x GCC backend (the Makefile is analysis-only, like
 * the S3 motorware). The peripheral register structures (CpuSysRegs, AdcaRegs,
 * EPwm1Regs, EQep1Regs, CanaRegs, SciaRegs, GpioCtrlRegs, ...) and the EALLOW
 * mechanism are the TI **C2000Ware** F28004x device support (`F28004x_device.h`
 * + the per-peripheral headers); they are referenced here as the real firmware
 * does and would be satisfied by C2000Ware in a CCS build.
 *
 * C28x notes: word-addressed (16-bit char); the FPU/TMU float ops (R0H-R7H) are
 * modelled as plain `float` math; EALLOW/EDIS bracket writes to protected
 * configuration registers.
 */

#include <stdint.h>

/* ---- C28x protected-register access (EALLOW/EDIS) ---- */
#define EALLOW   __asm(" EALLOW")
#define EDIS     __asm(" EDIS")

/* ---- raw word MMIO access (C28x data space, word addresses) ---- */
#define HWREG(a)    (*(volatile uint32_t *)(a))
#define HWREGH(a)   (*(volatile uint16_t *)(a))

/* ---- F28004x peripheral register files (from C2000Ware device support) ----
 * Declared here as the firmware uses them; full bitfield unions live in the TI
 * F28004x_*.h headers. */
extern volatile struct CPU_SYS_REGS  CpuSysRegs;   /* 0x5D300 ClkCfg/CpuSys */
extern volatile struct DEV_CFG_REGS  DevCfgRegs;   /* 0x5D000 DevCfg */
extern volatile struct CLK_CFG_REGS  ClkCfgRegs;   /* 0x5D200 ClkCfg */
extern volatile struct ADC_REGS      AdcaRegs, AdcbRegs, AdccRegs;       /* 0x7400/0x7480/0x7500 */
extern volatile struct ADC_RESULT_REGS AdcaResultRegs, AdcbResultRegs, AdccResultRegs; /* 0x0B00.. */
extern volatile struct EPWM_REGS     EPwm1Regs, EPwm2Regs, EPwm3Regs, EPwm4Regs;
extern volatile struct EPWM_REGS     EPwm5Regs, EPwm6Regs, EPwm7Regs, EPwm8Regs;        /* 0x4000+ */
extern volatile struct EQEP_REGS     EQep1Regs, EQep2Regs;               /* 0x5100/0x5140 */
extern volatile struct SCI_REGS      SciaRegs, ScibRegs;                 /* 0x7200/0x7210 */
extern volatile struct SPI_REGS      SpiaRegs, SpibRegs;                 /* 0x6100/0x6110 */
extern volatile struct CAN_REGS      CanaRegs;                           /* 0x48000 */
extern volatile struct GPIO_CTRL_REGS GpioCtrlRegs;                      /* 0x7C00 */
extern volatile struct GPIO_DATA_REGS GpioDataRegs;                      /* 0x7F00 */
extern volatile struct PIE_CTRL_REGS PieCtrlRegs;                        /* 0x0CE0 */
extern volatile struct CPU_TIMER_REGS CpuTimer0Regs, CpuTimer1Regs, CpuTimer2Regs;      /* 0x0C00.. */

/* ---- modelled config structs the firmware passes by pointer ---- */
typedef struct { volatile uint32_t *adcA, *adcB, *adcC; } AdcConfig;

/* ---- vendor / ROM / library helpers (TI RTS + boot ROM) ---- */
extern void delay_cycles(uint32_t n);     /* unk_E65A style busy-wait */
extern void codestart_0(void);            /* C-runtime _c_int00 */

/* The 87 reconstructed functions are declared in their source files; cross-file
 * callers use the names in docs/function_map.md. (Reference build: not compiled.) */

#endif /* MOTOR_CONTROL_H */
