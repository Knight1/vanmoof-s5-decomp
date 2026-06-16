/*
 * main.c — main_SystemInit, the C-runtime entry of the user_ecu firmware.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * One faithful translation of the OEM function @0x000044c0. Structure, register
 * offsets, magic constants, task-creation params and the SysTick math are kept
 * verbatim, each with a // 0x.... source comment. Every CALL is a separate
 * function (declared, not inlined). Direct MMIO is reproduced verbatim as
 * volatile accesses at the absolute peripheral addresses.
 *
 * Decompiler artifacts (phantom regs in the VFP charger block, the stale-reg
 * 2nd arg to FUN_00006a10, the unreachable post-scheduler tail) are documented
 * in main.c's open issues / comments and resolved to the real semantics.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "main.h"

/* Already-translated VanMoof helpers (call by name). */
#include "gpio.h"   /* gpio_pin_config @0x65a0 */
#include "pcc.h"    /* port_clock_wait @0x84cc, nvic_irq_enable @0x78d4,
                       gpio_base_to_bank @0x1ffc, pcc_gate_enable @0x8a64 */
#include "clock.h"  /* GetSystemCoreClockSource @0x11c0, Adc_ReadCh_LPO1MHz @0x1170,
                       periph_clk_nvic_enable @0x7988 */
#include "hal.h"    /* irqn_to_gpio_index @0x20e4, GetClock_32k @0x1188 */
#include "util.h"   /* vmem_set @0x9866 */

/* ---------------------------------------------------------------------------
 * VanMoof functions NOT yet translated — declared extern, exact Ghidra names.
 * ------------------------------------------------------------------------- */
extern void  SystemClock_PllFlashInit(void);                /* 0x00001244 */
extern void *FUN_00006a10(uint32_t size);                   /* 0x00006a10 (malloc-like; 2nd "arg" in decompile is a stale-register artifact) */
extern void  FUN_000087f2(void *p);                         /* 0x000087f2 */
extern void  FUN_00006b9c(void);                            /* 0x00006b9c */
extern void *FUN_000095be(uint32_t a, uint32_t b);          /* 0x000095be */
extern void  FUN_00003cd0(int a, void *b);                  /* 0x00003cd0 */
extern void  FUN_00008c8a(void *p);                         /* 0x00008c8a */
extern void  FUN_000079c0(void *p);                         /* 0x000079c0 */
extern int   thunk_FUN_0000974e(void);                      /* 0x000087a4 -> 0x0000974e */
extern int   FUN_0000974e(void);                            /* 0x0000974e */
extern int   registry_add(uint32_t busbase, void *cfg);     /* 0x00008594 */
extern int   FUN_000015e0(uint32_t a, int b, int c, int d, uint32_t e); /* 0x000015e0 */
extern int   FUN_00009178(uint32_t a, void *b);             /* 0x00009178 */
extern void  FUN_00003eac(uint32_t dst, int tag, int n, ...); /* 0x00003eac (frame builder, variadic) */
extern void *registry_lookup(uint32_t busbase, void *cfg);     /* 0x000082f2 */
extern int   xQueueSemaphoreTake(uint32_t a, int b);               /* 0x00008d54 */
extern int   registry_lookup_value(int a, uint32_t b);               /* 0x0000830a */
extern void  rtos_sem_give_dispatch(uint32_t a);            /* 0x000097f4 (FreeRTOS sem give, vendor) */
extern int   FUN_0000289c(int p);                           /* 0x0000289c */
extern void  FUN_0000664c(int a, uint32_t b);               /* 0x0000664c */
extern int   device_read_record87(uint32_t a, int b);       /* 0x00008e76 */
extern uint32_t device_read_record91(uint32_t a, int b);    /* 0x00008fd6 */
extern void  busy_wait(uint32_t a);                      /* 0x000084b2 */
extern void  gpio_irq_register(int bank, int pin, int idx, int seq); /* 0x0000159c */
extern int   FUN_000022b0(uint32_t base, int mode);         /* 0x000022b0 */
extern int   func_0x00002754(uint32_t a, void *b, uint32_t c); /* 0x00002754 */
extern int   FUN_000078f0(uint32_t a, uint32_t b, uint32_t c); /* 0x000078f0 */
extern int   FUN_0000879c(void);                            /* 0x0000879c */
extern int   func_0x00006708(int a, void *b);               /* 0x00006708 (raw indirect target; distinct from gpio_base_to_bank/pcc_gate/irqn labels) */
extern int   func_0x00009876(uint32_t a, void *b, int c);   /* 0x00009876 */
extern void  func_0x00001884(int a, int b, void *c, int d); /* 0x00001884 */
extern void  FUN_00003338(int p);                           /* 0x00003338 */
/* clock_div_program (0x0000110c) is declared by clock.h (already included). */
extern void  FUN_00006b44(void);                            /* 0x00006b44 */

/* NOTE: the OEM's PRIMASK critical section (mrs/cpsid/msr) and its VFP int<->float
 * conversions (vcvt) are CPU intrinsics, NOT function calls. Ghidra rendered them as
 * isCurrentModePrivileged()/VectorSignedToFloat()/... stubs that do not exist in the
 * firmware; they are reconstructed inline (asm / C casts) at their use sites below. */

/* ---------------------------------------------------------------------------
 * Vendor: FreeRTOS / scheduler-start primitives (not reconstructed).
 * ------------------------------------------------------------------------- */
extern int  xTaskCreate(void *entry, const char *name, uint16_t stack,
                        void *param, uint32_t prio, void *handle); /* vendor 0x00003144 */
extern void vPortRaiseBASEPRI(void);                        /* vendor 0x0000950a */
extern void vPortStartFirstTask_ControlTask(void);          /* vendor 0x00000370 vPortStartFirstTask */
extern void vTaskSwitchContext_SelectNext(void);            /* vendor 0x000063c0 */
extern void FUN_0000642c(void);                             /* vendor 0x0000642c */
extern void FUN_00000820(void);                             /* vendor 0x00000820 */

/* ---------------------------------------------------------------------------
 * Literal-pool constants and SRAM globals referenced by the OEM body.
 * These are the values/pointers loaded from the function's PC-relative pool;
 * the actual definitions live in startup/linker rodata/RAM. Declared extern.
 * ------------------------------------------------------------------------- */
extern int      DAT_00004840;          /* PORT/GPIO config base */
extern int     *DAT_00004844;          /* SCG/PCC struct base (puVar18) */
extern uint32_t DAT_00004848;
extern int      DAT_0000484c;          /* peripheral base */
extern uint32_t DAT_00004850;
extern uint32_t DAT_00004854;
extern int      DAT_00004858;          /* heap/region base */
extern uint32_t DAT_0000485c;
extern uint32_t DAT_00004860;
extern const char *DAT_00004864;       /* task name ptr (-> 0x0000a009) */
extern uint32_t DAT_00004874;          /* PCR clear mask */
extern void *PTR_LAB_00003c90_1_00004868;
extern void *PTR_LAB_000048f2_3_0000486c;
extern void *PTR_DAT_00004870;
extern uint32_t DAT_00004930;
extern int     *DAT_00004934;          /* piVar6 */
extern void    *DAT_00004938;
extern uint32_t DAT_0000493c;
extern uint32_t DAT_00004940;
extern uint32_t DAT_00004944;
extern uint32_t DAT_00004c08;
extern uint32_t DAT_00004c0c;
extern uint32_t DAT_00004c10;
extern uint32_t DAT_00004c14;
extern uint32_t DAT_00004c18;
extern int      DAT_00004c1c;
extern uint32_t DAT_00004c20;
extern uint32_t DAT_00004c24;
extern uint32_t *DAT_00004c28;
extern int      DAT_00004c2c;
extern int      DAT_00004c30;          /* PWM duty regs base */
extern uint32_t DAT_00004c34;
extern void    *DAT_00004c38;          /* task name (0x00004101) */
extern void    *DAT_00004c3c;          /* task entry (0x0001b2fb) */
extern int     *DAT_00004c40;
extern int     *DAT_00004ef8;          /* piVar6 (bus state) */
extern uint32_t DAT_00004efc;
extern uint32_t DAT_00004f00;          /* I2C bus base */
extern int     *DAT_00004f04;
extern uint32_t DAT_00004f08;
extern uint32_t DAT_00004f0c;
extern uint32_t DAT_00004f10;
extern int      DAT_00004f14;
extern uint32_t DAT_00004f18;
extern uint32_t DAT_00004f1c;
extern int      DAT_00004f20;
extern uint32_t DAT_00004f24;
extern uint32_t DAT_00004f28;
extern uint32_t DAT_000052d4;
extern uint32_t DAT_000052d8;
extern uint32_t DAT_000052dc;
extern uint32_t DAT_000052e0;          /* second I2C/IOM bus base */
extern uint32_t DAT_000052e4;
extern uint32_t DAT_000052e8;
extern uint32_t DAT_000052ec;
extern uint32_t DAT_000052f0;
extern uint32_t DAT_000052f4;
extern uint32_t DAT_000052f8;
extern uint32_t DAT_000052fc;
extern uint32_t DAT_00005300;
extern uint32_t *DAT_00005304;
extern uint32_t DAT_00005308;
extern uint32_t DAT_0000530c;
extern uint32_t DAT_00005310;
extern uint32_t DAT_00005314;
extern uint32_t DAT_00005318;
extern uint32_t DAT_0000531c;
extern uint32_t DAT_00005320;
extern uint32_t DAT_00005324;
extern uint32_t *DAT_00005650;         /* charger/PMIC status regs (puVar7) */
extern uint32_t uRam00005654;
extern uint32_t DAT_00005664;
extern int     *piRam00005658;
extern uint32_t uRam0000565c;
extern int    **piRam00005660;
extern uint32_t uRam00005668;
extern uint32_t uRam0000566c;
extern uint8_t  uRam00005670;
extern uint32_t *puRam00005674;
extern int      DAT_00005678;
extern int      DAT_0000567c;
extern uint32_t DAT_00005680;
extern uint32_t *DAT_00005684;
extern int      DAT_00005688;
extern uint32_t DAT_0000568c;
extern uint32_t DAT_00005690;
extern uint32_t DAT_00005694;
extern int      DAT_00005954;
extern uint32_t DAT_00005958;
extern uint32_t *DAT_0000595c;
extern uint32_t DAT_00005960;
extern uint32_t DAT_00005964;
extern uint32_t DAT_00005968;          /* L/R LED ring task param/base (0x20001734) */
extern void    *DAT_0000596c;          /* l_led_ring_task name (0x0001b2ff) */
extern void    *DAT_00005970;          /* l_led_ring_task entry (0x0000473d) */
extern void    *DAT_00005974;          /* r_led_ring_task name (0x0001b30f) */
extern void    *DAT_00005978;          /* r_led_ring_task entry (0x00004769) */
extern uint32_t DAT_0000597c;
extern uint32_t DAT_00005980;
extern uint32_t DAT_00005984;
extern uint32_t DAT_00005988;
extern uint32_t DAT_0000598c;
extern uint32_t DAT_00005990;
extern uint32_t DAT_00005994;
extern uint32_t DAT_00005998;
extern uint32_t DAT_0000599c;
extern uint32_t DAT_000059a0;
extern char    *DAT_000059a4;
extern uint32_t DAT_000059a8;
extern int     *DAT_000059ac;
extern uint32_t DAT_00005c90;
extern uint32_t DAT_00005c94;
extern uint32_t DAT_00005c98;
extern uint32_t DAT_00005c9c;
extern uint32_t DAT_00005ca0;
extern uint32_t *DAT_00005ca4;
extern uint32_t DAT_00005ca8;
extern int     *DAT_00005cac;
extern uint32_t DAT_00005cb0;
extern int      DAT_00005cb4;          /* DMIC/I2S peripheral base */
extern uint32_t DAT_00005cb8;
extern uint32_t DAT_00005cbc;
extern uint32_t DAT_00005cc0;
extern uint32_t DAT_00005cc4;
extern uint32_t DAT_00005cc8;
extern int      DAT_00005ccc;
extern uint32_t DAT_00005cd0;
extern int      DAT_00005cd4;
extern uint32_t DAT_00005cd8;
extern int     *DAT_00005cdc;
extern uint32_t DAT_00005ce0;
extern uint32_t DAT_00005ce4;
extern uint32_t DAT_00005ce8;
extern void    *DAT_00006000;          /* audio/DMIC-mgmt task entry (0x0001b325) */
extern uint32_t DAT_00005ff8;
extern uint32_t DAT_00005ffc;          /* task param/base (0x20001734) */
extern uint32_t DAT_00006004;
extern uint32_t DAT_00006008;
extern uint32_t DAT_0000600c;
extern uint32_t DAT_00006010;
extern int     *DAT_00006014;          /* puVar18 (CAN config) */
extern uint32_t DAT_00006018;
extern int      DAT_0000601c;
extern uint32_t DAT_00006020;
extern int      DAT_00006024;
extern uint8_t *DAT_00006028;          /* CAN bitrate descriptor (pbVar3) */
extern uint32_t *DAT_0000602c;
extern uint32_t DAT_00006030;
extern uint32_t *DAT_00006034;          /* GPIO bank base (puVar7) */
extern int      DAT_00006038;
extern int      DAT_0000603c;
extern int      DAT_00006040;
extern int      DAT_00006044;
extern int      DAT_00006048;
extern uint32_t *DAT_0000604c;
extern void    *DAT_00006050;          /* dmic_task name (0x00004be9) */
extern void    *DAT_00006054;          /* dmic_task wrapper entry (0x0001b332) */
extern uint32_t DAT_00006058;
extern uint32_t DAT_0000605c;
extern uint32_t DAT_00006060;
extern uint32_t DAT_00006064;
extern uint32_t DAT_00006068;
extern uint32_t DAT_0000606c;
extern int      DAT_00006070;          /* FlexCAN base (iVar16) */
extern int      DAT_00006074;
extern uint32_t DAT_00006280;
extern uint32_t DAT_00006284;          /* device handle */
extern uint32_t DAT_00006288;
extern uint32_t DAT_0000628c;
extern int      DAT_00006290;
extern uint32_t *DAT_00006294;          /* final descriptor (puVar7) */
extern uint32_t DAT_00006298;
extern uint32_t DAT_0000629c;
extern uint32_t DAT_000062a0;
extern uint32_t DAT_000062a4;
extern uint32_t DAT_000062a8;
extern uint32_t DAT_000062ac;
extern uint32_t DAT_000062b0;
extern void    *DAT_000062b4;          /* comm/IOM task handle (0x20001db8) */
extern const char *DAT_000062b8;       /* comm/IOM task name (0x0001b33c) */
extern void    *DAT_000062bc;          /* comm/IOM task entry (0x000078dd) */
extern int     *DAT_000062c0;          /* control-task presence flag */
extern void    *DAT_000062c4;          /* control task handle (0x20001e4c) */
extern const char *DAT_000062c8;       /* control task name (0x0001b341) */
extern void    *DAT_000062cc;          /* control task entry (0x00007c89) */
extern uint32_t *DAT_000062d0;         /* (SRAM) */
extern uint32_t *DAT_000062d4;
extern uint32_t *DAT_000062d8;
extern uint32_t *DAT_000062dc;         /* &SystemCoreClock (Hz) */
extern uint32_t *DAT_000062e0;
extern uint16_t  uRam00000bde;

/* ---------------------------------------------------------------------------
 * MMIO accessor macros (verbatim VanMoof glue).
 * ------------------------------------------------------------------------- */
#define MMIO32(addr) (*(volatile uint32_t *)(addr))
#define MMIO8(addr)  (*(volatile uint8_t  *)(addr))

void main_SystemInit(void)
{
    /* OEM locals (names follow the decompiler to preserve structure). */
    uint8_t  cfg0, cfg0b;
    uint8_t  cfg1, cfg1b;
    uint8_t  cfg2, cfg2b;
    uint16_t cfg3;
    uint16_t cfg4;
    uint16_t cfg5;
    int iVar14, iVar15, iVar16, iVar24, iVar32;
    int *piVar6;
    int *piVar13;
    uint32_t *puVar7;
    uint8_t  *pbVar3;
    uint32_t uVar17, uVar19, uVar21, uVar22, uVar27, uVar28, uVar29, uVar30, uVar31, uVar33, uVar34;
    int *piVar18;
    void *puVar18v;
    uint32_t uVar20;
    uint8_t  bVar10;
    char cVar11, cVar12;
    bool bVar35;
    float fVar36;

    /*
     * Config descriptor stack block (matches the OEM local_3c..uStack_14, one
     * contiguous on-stack descriptor handed to registry_add / registry_lookup etc.
     * as &local_3c). Modeled as one array so the address-of genuinely covers
     * every field, exactly as the OEM stack frame does.
     */
    uint32_t cfg[11];
#define local_3c   (cfg[0])
#define local_38   (cfg[1])
#define local_34   (cfg[2])
#define local_30   (cfg[3])
#define local_2c   (cfg[4])
#define local_28   (cfg[5])
#define local_24   (cfg[6])
#define local_20   (cfg[7])
#define local_1c   (cfg[8])
#define local_18   (cfg[9])
#define uStack_14  (cfg[10])
    uint32_t local_40, local_44;

    /* -------------------------------------------------------------------
     * 0x44c0 — Early GPIO pin-mux + initial port config
     * ----------------------------------------------------------------- */
    MMIO32(0x40000220) = 0x4000;                 /* 0x44c0 PCC/clock-gate strobe */
    cfg0 = 1; cfg0b = 0;
    {
        char b[2] = { (char)cfg0, (char)cfg0b };
        gpio_pin_config(4, b);                   /* 0x44c0 */
    }
    cfg1 = 1; cfg1b = 0;
    {
        char b[2] = { (char)cfg1, (char)cfg1b };
        gpio_pin_config(9, b);
    }
    cfg2 = 0; cfg2b = 0;
    {
        char b[2] = { (char)cfg2, (char)cfg2b };
        gpio_pin_config(10, b);
    }
    cfg3 = 0;                                     /* local_44 low half = 0 */
    gpio_pin_config(0x10, (const char *)&cfg3);
    cfg4 = 1;                                     /* local_40 low half = 1 */
    gpio_pin_config(0x17, (const char *)&cfg4);
    cfg5 = 0x101;                                 /* local_3c low half = 0x101 */
    gpio_pin_config(0x19, (const char *)&cfg5);

    iVar16 = DAT_00004840;
    MMIO32(DAT_00004840 + 0xc0) = 0x24;           /* 0x44c0 PORT mux fields */
    MMIO32(iVar16 + 0xc4) = 0x16;
    MMIO32(iVar16 + 0xc8) = 0x15;
    MMIO32(iVar16 + 0xcc) = 0x25;
    MMIO32(iVar16 + 0xd0) = 0x0f;
    /* PCR-style RMW run: clear pin-mux field, OR new mux value. */
    MMIO32(iVar16 - 0x4fd8) = (MMIO32(iVar16 - 0x4fd8) & 0xfffffef0) | 0x100;
    uVar17 = DAT_00004874;
    MMIO32(iVar16 - 0x4fcc) = (MMIO32(iVar16 - 0x4fcc) & DAT_00004874) | 0x101;
    MMIO32(iVar16 - 0x4fc8) = (MMIO32(iVar16 - 0x4fc8) & uVar17) | 0x101;
    MMIO32(iVar16 - 0x4fc4) = (MMIO32(iVar16 - 0x4fc4) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4fc0) = (MMIO32(iVar16 - 0x4fc0) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4ff8) = (MMIO32(iVar16 - 0x4ff8) & uVar17) | 0x101;
    MMIO32(iVar16 - 0x4fac) = (MMIO32(iVar16 - 0x4fac) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4fa8) = (MMIO32(iVar16 - 0x4fa8) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4fa4) = (MMIO32(iVar16 - 0x4fa4) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4f9c) = (MMIO32(iVar16 - 0x4f9c) & 0xfffffef0) | 0x100;
    piVar18 = DAT_00004844;
    MMIO32(iVar16 - 0x4f98) = (MMIO32(iVar16 - 0x4f98) & uVar17) | 0x109;
    MMIO32(iVar16 - 0x4ff4) = (MMIO32(iVar16 - 0x4ff4) & 0xfffffef0) | 0x101;
    MMIO32(iVar16 - 0x4ff0) = (MMIO32(iVar16 - 0x4ff0) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4fe8) = (MMIO32(iVar16 - 0x4fe8) & 0xfffffef0) | 0x101;
    MMIO32(iVar16 - 0x4fdc) = (MMIO32(iVar16 - 0x4fdc) & 0xfffffaf0) | 0x100;
    MMIO32(iVar16 - 0x4f78) = (MMIO32(iVar16 - 0x4f78) & 0xfffffef0) | 0x101;
    MMIO32(iVar16 - 0x4f2c) = (MMIO32(iVar16 - 0x4f2c) & 0xfffffef0) | 0x105;
    MMIO32(iVar16 - 0x4f74) = (MMIO32(iVar16 - 0x4f74) & 0xfffffef0) | 0x101;
    MMIO32(iVar16 - 0x4f70) = (MMIO32(iVar16 - 0x4f70) & 0xfffffef0) | 0x100;
    MMIO32(iVar16 - 0x4f6c) = (MMIO32(iVar16 - 0x4f6c) & 0xfffffef0) | 0x100;

    /* -------------------------------------------------------------------
     * 0x4570 — PLL/flash clock init + SCG/PCC follow-up
     * ----------------------------------------------------------------- */
    SystemClock_PllFlashInit();                   /* 0x4570 -> 0x1244 */
    piVar18[5] = 0;
    piVar18[6] = 0;
    piVar18[7] = 0;
    MMIO32(0x40000220) = 0x40000;
    port_clock_wait(0x12);
    iVar16 = DAT_0000484c;
    MMIO32(0x40004030) = DAT_00004848;
    MMIO8(DAT_0000484c + 0x305) = 0x40;
    MMIO8(iVar16 + 0x306) = 0x40;
    MMIO8(iVar16 + 0x307) = 0x40;
    MMIO8(iVar16 + 0x320) = 0x40;                 /* 800 = 0x320 */
    MMIO8(iVar16 + 0x304) = 0x60;
    uVar20 = DAT_00004850;
    piVar18[1] = (int)uVar20;
    piVar18[2] = (int)uVar20;
    piVar18[3] = (int)uVar20;
    MMIO32(0x40004000) = MMIO32(0x40004000) & 0xfffffff1;
    piVar18[4] = (int)uVar20;
    MMIO32(0x40004000) = MMIO32(0x40004000) & 0xffffffef;
    MMIO32(0x40004008) = 0x10;
    MMIO32(0x4000401c) = 1;
    MMIO32(0x40004020) = 1;
    *piVar18 = (int)DAT_00004854;
    iVar16 = DAT_00004858;
    MMIO32(0x40004000) = MMIO32(0x40004000) & 0xfffffffe;
    MMIO32(0x4000400c) = 1;
    MMIO32(0x40004014) = 1;

    /* -------------------------------------------------------------------
     * 0x45f4 — Peripheral clock-gate / NVIC enable + heap/region zero
     * ----------------------------------------------------------------- */
    periph_clk_nvic_enable(1);
    periph_clk_nvic_enable(2);
    periph_clk_nvic_enable(3);
    periph_clk_nvic_enable(4);
    periph_clk_nvic_enable(0);
    vmem_set((void *)iVar16, 0, 0x5b0);
    vmem_set((void *)iVar16, 0, 0x10);
    *(int *)(iVar16 + 0xc) = iVar16 + 0x10;
    *(uint32_t *)(iVar16 + 4) = 0x20;
    *(uint32_t *)(iVar16 + 8) = DAT_0000485c;

    /* -------------------------------------------------------------------
     * 0x4626 — Allocator bring-up + first xTaskCreate (control/IOM task)
     * ----------------------------------------------------------------- */
    piVar13 = (int *)FUN_00006a10(8);
    if (piVar13 == NULL) {
LAB_00004826:
        for (;;) { }                              /* 0x4826 do-nothing error spin */
    }
    iVar14 = (int)FUN_00006a10(0xb61);
    if (iVar14 == 0) {
        *piVar13 = 0;
        goto LAB_00004826;
    }
    iVar24 = iVar14 + 0x20;
    vmem_set((void *)iVar24, 0x55, 0xb41);        /* fill, then verify dst+count */
    if (iVar24 != (iVar14 + 0x20 + 0xb41)) {
        vPortRaiseBASEPRI();
        for (;;) { }
    }
    vmem_set((void *)iVar14, 0, 0x20);
    MMIO8(iVar14 + 0x1c) = 1;
    *(int *)(iVar14 + 0x18) = iVar24;
    *(uint32_t *)(iVar14 + 8) = 0xb41;
    *(uint32_t *)(iVar14 + 0xc) = 1;
    *piVar13 = iVar14;
    iVar15 = (int)FUN_00006a10(0xc);
    *(uint32_t *)(iVar15 + 4) = DAT_00004860;
    *(int *)(iVar15 + 8) = iVar14;
    iVar14 = xTaskCreate(PTR_LAB_00003c90_1_00004868, DAT_00004864, 0x168,
                         (void *)iVar15, 2, (void *)iVar15);   /* prio 2, stack 0x168 */
    if (iVar14 != 1) {
        FUN_000087f2((void *)iVar15);
        iVar16 = *piVar13;
        piVar13[1] = 0;
        if (iVar16 != 0) {
            if ((int)((uint32_t)MMIO8(iVar16 + 0x1c) << 0x1e) < 0) {
                vmem_set((void *)iVar16, 0, 0x20);
            } else {
                FUN_00006b9c();
            }
        }
        FUN_000087f2((void *)*piVar13);
        goto LAB_00004826;
    }
    piVar13[1] = iVar15;
    *(void **)(iVar16 + 0x594) = PTR_LAB_000048f2_3_0000486c;
    *(int **)(iVar16 + 0x590) = piVar13;
    *(void **)(iVar16 + 0x598) = PTR_DAT_00004870;
    *(int *)(iVar16 + 0x5a0) = iVar16;
    *(int **)(iVar16 + 0x59c) = piVar13;
    if (*(int *)(iVar16 + 0x5ac) != 0) goto LAB_00004826;
    iVar14 = (int)FUN_00006a10(0xe4);
    *(int *)(iVar16 + 0x5ac) = iVar14;
    if (iVar14 == 0) goto LAB_00004826;
    *(uint32_t *)(iVar16 + 0x5a4) = DAT_00004930;
    piVar13 = (int *)FUN_00006a10(0x17c);
    *(int **)(iVar16 + 0x5a8) = piVar13;
    if (piVar13 == NULL) goto LAB_00004826;

    /* -------------------------------------------------------------------
     * 0x48f2 — ADC clock-source select + LED-PWM (FTM) configuration
     * ----------------------------------------------------------------- */
    if (MMIO32(0x400002a0) == 1) {                /* ADC clock-source select */
        uVar17 = Adc_ReadCh_LPO1MHz();
    } else if (MMIO32(0x400002a0) == 2) {
        uVar17 = GetClock_32k();
    } else if (MMIO32(0x400002a0) == 0) {
        uVar17 = GetSystemCoreClockSource();
        uVar17 = uVar17 / ((MMIO32(0x4000030c) & 0xff) + 1);
    } else {
        uVar17 = 0;
    }
    piVar6 = DAT_00004934;
    if (*DAT_00004934 == 0) {
        iVar14 = (int)FUN_000095be(0x80, 0x10);
        *piVar13 = iVar14;
        FUN_00003cd0(iVar14, DAT_00004938);
        piVar18 = (int *)FUN_00006a10(0x18);
        iVar14 = 0;
        if (piVar18 != NULL) {
            *piVar18 = 0;
            FUN_00008c8a(piVar18 + 1);
            iVar14 = piVar18[0]; /* OEM consumes returned r1; value lives in node[0] */
            /* NOTE: decompiler 'extraout_r1' — FUN_00008c8a leaves the new
             * timer/handle in r1; modeled as the node it just initialized. */
            iVar14 = (int)(intptr_t)piVar18;
        }
        piVar13[1] = (int)DAT_0000493c;
        piVar13[0x56] = (int)DAT_0000493c;
        MMIO8((int)(piVar13 + 0x5a)) = 0;
        piVar13[0x57] = (int)DAT_00004940;
        piVar13[0x58] = (int)DAT_00004944;
        if ((*piVar13 != 0) && (iVar14 != 0)) {
            /* large inline FTM/LPTMR-style config block (local_3c..) */
            vmem_set(&local_3c, 0, 0x1c);
            local_3c = DAT_00004c08;
            local_38 = DAT_00004c08;
            local_34 = 0;
            local_30 = local_30 & 0xffffff00;
            local_2c = (local_2c & 0xff000000) | 0x30000;
            local_2c = (local_2c & 0xff000000) | 0x00000a03; /* low 16 = 0xa03 */
            local_28 = (local_28 & 0x0000ffff) | (0x0a03u << 16);
            local_24 = (local_24 & 0xffffff00) | 3;
            uVar19 = local_28;
            if (uVar17 == DAT_00004c0c) {
                int iVar14f = piVar13[0x58];
                local_2c = (local_2c & 0xff000000) | 0x51005;
                local_28 = uVar19 & 0xffff;
                local_24 = (local_24 & 0xffffff00) << 8;
                MMIO32(0x40000224) = 0x80;
                port_clock_wait(DAT_00004c10);
                MMIO32(iVar14f + 0x18) |= 1;
                while (-1 < (int)(MMIO32(iVar14f + 0x18) << 0x1f)) { }
                MMIO32(iVar14f + 0x18) |= 2;
                if (((local_34 >> 16) & 0xff) != 0) {
                    MMIO32(iVar14f + 0x18) |= 0xa0;
                    MMIO32(iVar14f + 0x10) |= 0x10;
                }
                if (((local_34 >> 24) & 0xff) != 0) {
                    MMIO32(iVar14f + 0x18) |= 0x80;
                    MMIO32(iVar14f + 0x10) |= 0x10;
                }
                if ((local_30 & 0xff) != 0) {
                    MMIO32(iVar14f + 0x18) |= 0x20;
                }
                {
                    uint32_t hi = (local_2c >> 8) & 0xff;
                    uint32_t den = local_3c * (((local_2c >> 0x10) & 0xff) + hi + 3);
                    if (den == 0) den = 1;
                    den = DAT_00004c0c / den - 1;
                    MMIO32(iVar14f + 0x1c) &= 0x80;
                    if (0x1fe < den) den = 0x1ff;     /* clamp 0x1ff */
                    MMIO32(iVar14f + 0x1c) =
                        (hi << 8) | (local_2c << 0x19) | ((local_2c >> 0x10) & 0x7f)
                        | (den << 0x10) | MMIO32(iVar14f + 0x1c);
                }
                if ((local_34 & 0xff) != 0) {
                    MMIO32(iVar14f + 0x18) |= 0x100;
                }
                if (((local_34 >> 8) & 0xff) != 0) {
                    MMIO32(iVar14f + 0x18) |= 0x300;
                    {
                        uint32_t hi = local_28 >> 0x18;
                        uint32_t den = local_38 * (hi + (local_24 & 0xff) + 3);
                        if (den == 0) den = 1;
                        den = DAT_00004c0c / den - 1;
                        if (0x1e < den) den = 0x1f;   /* clamp 0x1f */
                        MMIO32(iVar14f + 0xc) &= DAT_00004c14;
                        MMIO32(iVar14f + 0xc) =
                            ((hi & 0x1f) << 8) | ((local_24 & 0xf) << 4)
                            | ((local_28 >> 0x10) & 0xf) | (den << 0x10)
                            | MMIO32(iVar14f + 0xc);
                        if ((((local_34 >> 16) & 0xff) == 0) &&
                            (((local_34 >> 24) & 0xff) == 0)) {
                            uint32_t v;
                            MMIO32(iVar14f + 0xc) |= 0x800000;
                            MMIO32(iVar14f + 0x48) &= 0xffff80ff;
                            v = (local_28 & 0xffff) * (hi + 2) + hi + 2;
                            if (v < 0x7f) {
                                v = MMIO32(iVar14f + 0x48) | (v * 0x100);
                            } else {
                                v = MMIO32(iVar14f + 0x48) | 0x7f00; /* clamp 0x7f00 */
                            }
                            MMIO32(iVar14f + 0x48) = v;
                        }
                    }
                }
                MMIO8((int)(piVar13 + 0x5a)) = 1;
                iVar14f = piVar13[0x58];
                vmem_set(piVar13 + 2, 0, 0x150);
                bVar35 = (iVar14f != (int)DAT_00004c18);
                *(int **)(DAT_00004c1c + (uint32_t)bVar35 * 4) = piVar13 + 2;
                piVar13[2] = (int)DAT_00004c20;
                piVar13[3] = (int)piVar13;
                *DAT_00004c28 = DAT_00004c24;
                MMIO32(iVar14f + 0x5c) |= 1;
                MMIO32(iVar14f + 0x58) &= 0xfc7fffff;
                MMIO32(iVar14f + 0x54) |= 0x3800000;
                nvic_irq_enable((int)*(char *)(DAT_00004c2c + (uint32_t)bVar35 * 2));
                *(uint32_t *)(DAT_00004c30 + 4) = 0x1000;
                iVar14f = piVar13[0x58];
                *(uint32_t *)(iVar14f + 0x200) = 0x4000000;
                MMIO32(iVar14f + 0xa0) |= 0x10010;
                MMIO32(iVar14f + 0xbc) = MMIO32(iVar14f + 0xbc);
                MMIO32(iVar14f + 0xc0) = DAT_00004c34 | MMIO32(iVar14f + 0xc0);
                MMIO32(iVar14f + 0xc8) = MMIO32(iVar14f + 0xc8); /* 200 = 0xc8 */
                if (*(char *)((int)piVar13 + 0x155) == '\0') {
                    *(uint8_t *)((int)piVar13 + 0x155) = 5;
                    piVar13[0x44] = (int)(piVar13 + 0x5b);
                    MMIO32(iVar14f + 0x5c) |= 1;
                    MMIO32(iVar14f + 0x58) &= 0xfffffffe;
                    MMIO32(iVar14f + 0x54) |= 1;
                }
                MMIO32(iVar14f + 0x18) &= 0xfffffffe;
                iVar15 = DAT_00004c30;
                while ((int)(MMIO32(iVar14f + 0x18) << 0x1f) < 0) { }
                *piVar6 = (int)piVar13;
                MMIO8(iVar15 + 0x32b) = 0x40;          /* PWM duty bytes */
                MMIO8(iVar15 + 0x32c) = 0x40;
                iVar14 = xTaskCreate(DAT_00004c3c, DAT_00004c38, 0x10e,
                                     (void *)piVar13, 3, piVar13 + 0x59); /* prio 3, stack 0x10e */
                if (iVar14 != 0) goto LAB_00004c00;
            }
        }
        FUN_000079c0(piVar13);                    /* cleanup on error */
    }

LAB_00004c00:
    /* -------------------------------------------------------------------
     * 0x4c00 — I2C/IOM bus + sensor device bring-up (registry_add sweep)
     * ----------------------------------------------------------------- */
    piVar6 = DAT_00004ef8;
    piVar13 = DAT_00004c40;
    iVar14 = *DAT_00004c40;
    if (iVar14 != 0) goto LAB_00004c06;
    vmem_set(DAT_00004ef8, 0, 4);
    *piVar6 = iVar16;
    *piVar13 = (int)piVar6;
    local_3c = (uint32_t)FUN_00006a10(0x1e);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 0x1e;
    local_30 = DAT_00004efc;
    local_2c = (local_2c & 0xffffff00) | 2;
    if ((local_38 == 0) || (local_3c == 0)) goto LAB_00004c06;
    local_28 = iVar14; local_24 = iVar14; local_20 = iVar14;
    local_1c = iVar14; local_18 = iVar14; uStack_14 = iVar14;
    iVar14 = registry_add(DAT_00004f00, &local_3c);
    piVar13 = DAT_00004f04;
    if (iVar14 != 0) goto LAB_00004c06;
    iVar15 = piVar6[10];
    if (iVar15 == 0) iVar15 = 1000;
    vmem_set(piVar6 + 2, 0, 0x28);
    piVar6[2] = iVar16;
    *piVar13 = (int)(piVar6 + 2);
    iVar24 = (int)FUN_00006a10(0xc);
    piVar6[4] = iVar24;
    if (iVar24 == 0) goto LAB_00004c06;
    iVar32 = *piVar13;
    iVar24 = thunk_FUN_0000974e();
    *(int *)(iVar32 + 4) = iVar24;
    if (iVar24 == 0) goto LAB_00004c06;
    local_3c = (uint32_t)FUN_00006a10(8);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 8;
    local_30 = 0xc08808;
    local_2c = (local_2c & 0xffffff00) | 2;
    if ((local_38 == 0) || (local_3c == 0)) goto LAB_00004c06;
    local_28 = iVar14; local_24 = iVar14; local_20 = iVar14;
    local_1c = iVar14; local_18 = iVar14; uStack_14 = iVar14;
    iVar14 = registry_add(DAT_00004f00, &local_3c);
    if (iVar14 != 0) goto LAB_00004c06;
    iVar14 = FUN_000015e0(*(uint32_t *)(*piVar13 + 8), iVar15, 0, 0, DAT_00004f08);
    if (iVar14 != 0) goto LAB_00004c06;
    iVar14 = FUN_00009178(*(uint32_t *)*piVar13, (uint32_t *)(*piVar13) + 4);
    if (iVar14 != 0) goto LAB_00004c06;
    vmem_set(&DAT_00004f0c, 0, 8);
    MMIO8((int)(piVar6 + 0x14)) = 0xc0;
    piVar6[0x13] = iVar16;
    local_3c = (uint32_t)FUN_00006a10(7);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 7;
    local_30 = DAT_00004f10;
    local_2c = (local_2c & 0xffffff00);
    local_28 = iVar14; local_24 = iVar14; local_20 = iVar14;
    local_1c = iVar14; local_18 = iVar14; uStack_14 = iVar14;
    if ((local_38 == 0) || (local_3c == 0)) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(DAT_00004f00, &local_3c);
    }
    {
        uint16_t uVar4 = uRam00000bde;
        uVar17 = *(uint32_t *)(DAT_00004f14 + 4);
        FUN_00003eac(DAT_00004f18, 0x18, 4, 0x35, (int)uVar4,
                     (int)(uVar17 >> 0x10), (int)(uVar17 & 0xffff));
        local_3c = (local_3c & 0xff000000) | 0x80c0; /* low halfword 0x80c0 */
        puVar18v = registry_lookup(DAT_00004f00, &local_3c);
        if (puVar18v == NULL) goto LAB_00004c06;
        iVar15 = xQueueSemaphoreTake(((uint32_t *)puVar18v)[1], 100);
        if (iVar15 != 0) goto LAB_00004c06;
        {
            uint8_t *puVar25 = (uint8_t *)*(uint32_t *)puVar18v;
            *puVar25 = 0x35;
            *(uint16_t *)(puVar25 + 1) = uVar4;
            *(int16_t *)(puVar25 + 3) = (int16_t)(uVar17 >> 0x10);
            *(int16_t *)(puVar25 + 5) = (int16_t)uVar17;
        }
        local_3c = (local_3c & 0xff000000) | 0x80c0;
        iVar15 = (int)(intptr_t)registry_lookup(DAT_00004f00, &local_3c);
        if (iVar15 == 0) goto LAB_00004c06;
        rtos_sem_give_dispatch(*(uint32_t *)(iVar15 + 4));
        if (iVar14 != 0) goto LAB_00004c06;
    }
    vmem_set(&DAT_00004f1c, 0, 0x1c);
    piVar6[0xc] = iVar16;
    MMIO8((int)(piVar6 + 0xd)) = 0xc0;
    iVar16 = (int)FUN_00006a10(0x48);
    if (iVar16 == 0) {
LAB_00004e4e:
        piVar6[0x12] = 0;
        iVar16 = -1;
    } else {
        iVar14 = (int)FUN_00006a10(0x200);
        *(int *)(iVar16 + 0x40) = iVar14;
        if (iVar14 == 0) {
LAB_00004eb0:
            FUN_000087f2((void *)iVar16);
            goto LAB_00004e4e;
        }
        iVar14 = FUN_0000289c(iVar16);
        if (iVar14 != 0) {
            FUN_000087f2((void *)*(uint32_t *)(iVar16 + 0x40));
            goto LAB_00004eb0;
        }
        MMIO8(iVar16 + 0x3c) = 1;
        iVar14 = (*(int (**)(int, uint32_t, int))(*(int *)(DAT_00004f20 + 0x10) + 0x10))
                     (iVar16, DAT_00004f24, 0x200); /* indirect HAL read */
        if (iVar14 != 0) {
            FUN_00003eac(DAT_00004f28, 0x65, 0);
            FUN_0000664c(iVar16, DAT_00004f24);
        }
        piVar6[0x12] = iVar16;
        iVar16 = 0;
    }
    {
        int iVar14d = piVar6[0xd];
        iVar15 = piVar6[0xc];
        local_3c = (uint32_t)FUN_00006a10(4);
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 4;
        local_28 = 0; local_24 = 0; local_20 = 0;
        local_1c = 0; local_18 = 0; uStack_14 = 0;
        local_30 = (uint32_t)((1u << 24) | (((uint32_t)(uint8_t)iVar14d) << 16)
                              | (0x81u << 8) | (uint8_t)iVar14d);
        local_2c &= 0xffffff00;
        if ((local_38 == 0) || (local_3c == 0)) {
LAB_00004e98:
            iVar14 = -1;
        } else {
            iVar14 = registry_add(iVar15, &local_3c);
            if (iVar14 == 0) {
                if ((piVar6[0x12] != 0) && (*(char *)(piVar6[0x12] + 0x3c) != '\0')) {
                    void *p;
                    piVar6[0xe] = 0xda0000;
                    *(uint16_t *)(piVar6 + 0xf) = 0x200;
                    local_3c = (local_3c & 0xff000000) | 0x8100;
                    local_3c = (local_3c & 0xffffff00) | (uint8_t)piVar6[0xd];
                    *(uint16_t *)((int)piVar6 + 0x36) = 0;
                    piVar6[0x10] = 0;
                    *(uint16_t *)(piVar6 + 0x11) = 0;
                    p = (void *)(intptr_t)registry_lookup_value(piVar6[0xc], local_3c);
                    if (p != NULL) {
                        iVar24 = xQueueSemaphoreTake(((uint32_t *)p)[1], 100);
                        iVar15 = (int)local_3c;
                        if (iVar24 == 0) {
                            uint16_t *puVar26 = (uint16_t *)*(uint32_t *)p;
                            if (puVar26 != NULL) {
                                puVar26[0] = (uint16_t)piVar6[0xf];
                                puVar26[1] = *(uint16_t *)((int)piVar6 + 0x3a);
                            }
                            local_3c = (local_3c & 0xffff0000)
                                       | (uint16_t)((0x81u << 8) | (uint8_t)piVar6[0xd]);
                            local_3c = (local_3c & 0x00ffffff)
                                       | (((uint32_t)iVar15 >> 24) << 24);
                            iVar15 = registry_lookup_value(piVar6[0xc], local_3c);
                            if (iVar15 != 0) {
                                rtos_sem_give_dispatch(*(uint32_t *)(iVar15 + 4));
                                goto LAB_00004fbc;
                            }
                        }
                    }
                }
                goto LAB_00004e98;
            }
        }
    }
LAB_00004fbc:
    if (iVar16 != 0) iVar14 = -1;
    local_3c = (uint32_t)FUN_00006a10(4);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    uVar20 = DAT_000052d8;
    local_34 = 4;
    local_30 = DAT_000052d4;
    local_2c = (local_2c & 0xffffff00) | 2;
    local_28 = DAT_000052dc;
    local_24 = DAT_000052d8;
    local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
    if ((local_38 == 0) || (local_3c == 0)) {
        iVar16 = -1;
    } else {
        iVar16 = registry_add(DAT_000052e0, &local_3c);
    }
    local_3c = (uint32_t)FUN_00006a10(8);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    if (iVar14 != 0) iVar16 = iVar14;
    local_34 = 8;
    local_30 = DAT_000052e4;
    local_2c = (local_2c & 0xffffff00) | 2;
    local_28 = DAT_000052e8;
    local_24 = uVar20;
    local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
    if ((local_38 == 0) || (local_3c == 0)) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(DAT_000052e0, &local_3c);
    }
    local_3c = (uint32_t)FUN_00006a10(4);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    if (iVar16 != 0) iVar14 = iVar16;
    local_34 = 4;
    local_30 = DAT_000052ec;
    local_28 = DAT_000052f0;
    local_24 = uVar20;
    local_2c &= 0xffffff00;
    local_20 = 0;
    local_1c = (uint32_t)FUN_00006a10(2);
    local_18 = 2;
    uStack_14 = 0;
    if ((local_38 == 0) || (local_3c == 0) || (local_1c == 0)) {
        iVar16 = -1;
    } else {
        iVar16 = registry_add(DAT_000052e0, &local_3c);
    }
    local_3c = (uint32_t)FUN_00006a10(1);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    if (iVar14 != 0) iVar16 = iVar14;
    local_34 = 1;
    local_30 = DAT_000052f4;
    local_28 = DAT_000052f8;
    local_24 = uVar20;
    local_2c &= 0xffffff00;
    local_20 = 0;
    local_1c = (uint32_t)FUN_00006a10(4);
    local_18 = 4;
    uStack_14 = 0;
    if ((local_38 == 0) || (local_3c == 0) || (local_1c == 0)) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(DAT_000052e0, &local_3c);
    }
    local_3c = 0;
    local_38 = (uint32_t)thunk_FUN_0000974e();
    if (iVar16 != 0) iVar14 = iVar16;
    local_30 = DAT_000052fc;
    local_2c = (local_2c & 0xffffff00) | 2;
    local_34 = 0;
    local_28 = DAT_00005300;
    local_24 = uVar20;
    local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
    if (local_38 == 0) {
        bVar10 = 0xff;
    } else {
        bVar10 = (uint8_t)registry_add(DAT_000052e0, &local_3c);
    }
    puVar7 = DAT_00005304;
    iVar16 = (int)(char)((uint8_t)iVar14 | bVar10);
    if (iVar16 != 0) {
LAB_00004c06:
        for (;;) { }                              /* 0x4c06 error spin */
    }
    vmem_set(DAT_00005304, 0, 8);
    uVar17 = DAT_000052e0;
    MMIO8((int)(puVar7 + 1)) = 0xc0;
    *puVar7 = uVar17;
    local_3c = (uint32_t)iVar16;
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_2c = (local_2c & 0xffffff00) | 1;
    local_34 = 1;
    local_30 = DAT_00005308;
    local_28 = DAT_0000530c;
    local_24 = (uint32_t)(intptr_t)puVar7;
    local_20 = iVar16; local_1c = iVar16; local_18 = iVar16; uStack_14 = iVar16;
    if (local_38 == 0) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(uVar17, &local_3c);
    }
    local_3c = (uint32_t)FUN_00006a10(0xd);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 0xd;
    local_30 = DAT_00005310;
    local_28 = 0; local_24 = 0; local_20 = 0;
    local_1c = 0; local_18 = 0; uStack_14 = 0;
    local_2c &= 0xffffff00;
    if ((local_38 == 0) || (local_3c == 0)) {
        iVar16 = -1;
    } else {
        iVar16 = registry_add(DAT_000052e0, &local_3c);
    }
    if (iVar14 != 0) iVar16 = iVar14;
    local_3c = 0;
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_20 = 0; local_1c = 0;
    local_34 = 0xd;
    local_30 = DAT_00005314;
    local_2c = (local_2c & 0xffffff00) | 1;
    local_18 = 0; uStack_14 = 0;
    local_28 = DAT_00005318;
    local_24 = (uint32_t)(intptr_t)puVar7;
    if (local_38 == 0) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(DAT_000052e0, &local_3c);
    }
    local_3c = (uint32_t)FUN_00006a10(0x10);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    if (iVar16 != 0) iVar14 = iVar16;
    local_34 = 0x10;
    local_30 = DAT_0000531c;
    local_28 = 0; local_24 = 0; local_20 = 0;
    local_1c = 0; local_18 = 0; uStack_14 = 0;
    local_2c &= 0xffffff00;
    if ((local_38 == 0) || (local_3c == 0)) {
        iVar16 = -1;
    } else {
        iVar16 = registry_add(DAT_000052e0, &local_3c);
    }
    if (iVar14 != 0) iVar16 = iVar14;
    local_3c = 0;
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_20 = 0; local_1c = 0;
    local_34 = 0x10;
    local_30 = DAT_00005320;
    local_2c = (local_2c & 0xffffff00) | 1;
    local_18 = 0; uStack_14 = 0;
    local_28 = DAT_00005324;
    local_24 = (uint32_t)(intptr_t)puVar7;
    if (local_38 == 0) {
        iVar14 = -1;
    } else {
        iVar14 = registry_add(DAT_000052e0, &local_3c);
    }
    iVar15 = device_read_record87((uint32_t)(intptr_t)DAT_00005304, 0);
    if (iVar16 != 0) iVar14 = iVar16;
    if (iVar14 == 0) iVar14 = iVar15;
    uVar19 = device_read_record91((uint32_t)(intptr_t)DAT_00005304, 0);
    puVar7 = DAT_00005650;
    if ((iVar14 != 0) || (uVar19 != 0)) goto LAB_00004c06;

    /* -------------------------------------------------------------------
     * 0x5654 — Power/charger sensor sampling + critical-section write
     * (Ghidra mis-bounds this region; instructions live out-of-line near
     *  0x535e..0x5554 — translated from disassembly there.)
     * ----------------------------------------------------------------- */
    MMIO32(0x40000a18) |= 0x40;                    /* 0x5362 */
    uVar28 = *DAT_00005650;                         /* 0x5366 status */
    MMIO32(0x4000038c) = 0x20000000;                /* 0x5376 */
    MMIO32(0x4000038c) = uVar19;                    /* 0x537c (uVar19 == 0) */
    vmem_set(&local_3c, 0, 0x14);
    local_3c = (local_3c & 0xff000000) | 1;
    local_38 = 0xffffff;
    local_34 = 0xffffff;
    local_30 = uVar19;
    local_2c = uVar19;
    /* first sample: scaled by 5.75f (vmov.f32 s14,0x40b80000 @0x53a6) */
    uVar19 = Adc_ReadCh_LPO1MHz();
    fVar36 = (float)(int32_t)((uVar19 / ((MMIO32(0x4000038c) & 0x3f) + 1)) >> 2); /* vcvt.f32.s32 */
    local_40 = (uint32_t)(fVar36 * 5.75f);                                        /* vcvt.u32.f32 (rtz) */
    local_34 = local_40;
    /* second sample: scaled by 1.5f (vmov.f32 s14,0x3fc00000 @0x53cc) */
    uVar19 = Adc_ReadCh_LPO1MHz();
    uVar20 = local_3c;
    local_3c = (local_3c & 0xffff00ff) | ((uint8_t)1 << 8); /* strb r2(=1) @0x53da */
    {
        uint32_t uVar9 = local_3c;
        fVar36 = (float)(int32_t)((uVar19 / ((MMIO32(0x4000038c) & 0x3f) + 1)) >> 2); /* vcvt.f32.s32 */
        local_44 = (uint32_t)(fVar36 * 1.5f);                                         /* vcvt.u32.f32 (rtz) */
        MMIO32(0x40000220) = 0x400000;
        if ((int)(*puVar7 << 0x1c) < 0) {
            uVar19 = (local_3c & 0xff) | 10;
        } else {
            uVar19 = (local_3c & 0xff) | 2;
        }
        {
            /* OEM critical section @0x5420..0x5448: mrs r1,primask; cpsid i;
             * <stores>; msr primask,r1. (Ghidra rendered the PRIMASK save/
             * disable/restore as isCurrentModePrivileged/...IRQinterrupts stubs;
             * reconstructed here as the real inline asm.) */
            uint32_t primask_save;
            __asm volatile ("mrs %0, primask" : "=r"(primask_save));
            __asm volatile ("cpsid i" ::: "memory");
            puVar7[1] = local_34 & 0xffffff;
            *puVar7 = uVar19;
            puVar7[6] = local_44 & 0xffffff;
            puVar7[5] = local_30 & 0x3ff;
            /* volatile-cast: same-address double write (0xaa unlock then 0x55) */
            ((volatile uint32_t *)puVar7)[2] = 0xaa;
            ((volatile uint32_t *)puVar7)[2] = 0x55;
            __asm volatile ("msr primask, %0" :: "r"(primask_save) : "memory");
        }
        if ((local_3c & 0xff) != 0) {
            while (((uint8_t *)DAT_00005650)[3] == 0xff) { } /* poll */
        }
        local_3c = (local_3c & 0xff00ffff) | (((uVar20 >> 16) & 0xff) << 16);
        bVar35 = (((local_3c >> 16) & 0xff) != 0);
        local_3c = uVar9;
        local_38 = local_44;
        if (bVar35 && ((uVar21 = (*DAT_00005650 << 0x1b), -1 < (int)uVar21))) {
            busy_wait(((uRam00005654 / local_2c) * 100 + 100) >> 2);   /* 0x5466..0x5476 */
            *(volatile uint32_t *)DAT_00005650 |= 0x10; /* 0x547a..0x5480 ldr/orr #0x10/str (verify: was dropped) */
        }
    }

    /* -------------------------------------------------------------------
     * 0x5781 — GPIO IRQ pin registrations + battery/comm timers
     * ----------------------------------------------------------------- */
    iVar16 = (int)FUN_00006a10(0xc);
    *piRam00005658 = iVar16;
    if (iVar16 != 0) {
        FUN_000015e0(iVar16, 5000, 1, 0, uRam0000565c); /* 5000 ms timer */
    }
    piVar13 = (int *)piRam00005660;
    if ((uVar28 & 4) != 0) {
        xQueueSemaphoreTake(*(uint32_t *)(*piRam00005660 + 4), 100);
        iVar16 = *piVar13;
        MMIO32(iVar16 + 0x10) |= 1;                 /* device wake */
        MMIO32(iVar16 + 0x18) |= 1;
        rtos_sem_give_dispatch(*(uint32_t *)(iVar16 + 4));
        FUN_00003eac(DAT_00005664, 0x41, 0);
    }
    local_3c = (uint32_t)FUN_00006a10(3);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 3;
    local_30 = uRam00005668;
    local_28 = 0; local_24 = 0; local_20 = 0;
    local_1c = 0; local_18 = 0; uStack_14 = 0;
    local_2c = (local_2c & 0xffffff00) | 2;
    if ((local_38 == 0) || (local_3c == 0) ||
        (iVar16 = registry_add(uRam0000566c, &local_3c), iVar16 != 0)) {
        FUN_00003eac(DAT_000062b0, 0x45, 0);
    } else {
        vmem_set(&uRam00005670, 0, 0x30);
        *puRam00005674 = uVar17;
        gpio_irq_register(0, 0x16, 1, 0);                /* GPIO-IRQ registrations */
        gpio_irq_register(0, 0x15, 2, 1);
        gpio_irq_register(1, 0x05, 3, 2);
        gpio_irq_register(0, 0x0f, 4, 3);
    }
    /* PWM channels x6 into iRam00005678 / iRam0000567c */
    *(uint32_t *)(DAT_00005678 + 8)     = (uint32_t)(intptr_t)FUN_000095be(1, 0);
    iVar16 = DAT_00005678;
    *(uint32_t *)(iVar16 + 0x134)        = (uint32_t)(intptr_t)FUN_000095be(1, 0);
    *(uint32_t *)(iVar16 + 0xc)          = (uint32_t)(intptr_t)FUN_000095be(4, 7);
    *(uint32_t *)(DAT_0000567c + 8)      = (uint32_t)(intptr_t)FUN_000095be(1, 0);
    iVar14 = DAT_0000567c;
    *(uint32_t *)(iVar14 + 0x134)        = (uint32_t)(intptr_t)FUN_000095be(1, 0);
    *(uint32_t *)(iVar14 + 0xc)          = (uint32_t)(intptr_t)FUN_000095be(4, 7);

    /* -------------------------------------------------------------------
     * 0x5951 — SAI/DMIC clock-tree + L/R LED-ring tasks
     * ----------------------------------------------------------------- */
    vmem_set(&local_3c, 0, 0x18);
    local_34 = DAT_00005680;
    local_30 = 7;
    local_2c = 0;
    local_28 &= 0xffff0000;
    local_38 &= 0xffffff00;
    local_3c = 0x1000100;                            /* SAI config */
    switch (MMIO32(0x400002d0)) {                    /* source-clock select */
    case 0:
        uVar19 = GetSystemCoreClockSource();
        break;
    case 1:
        uVar19 = *DAT_00005684 / ((MMIO32(0x400003c4) & 0xff) + 1);
        break;
    case 2:
        uVar19 = DAT_0000568c;
        if (-1 < (int)(MMIO32(DAT_00005688 + 0x10) << 0x11)) goto switchD_caseD_5;
        goto LAB_00005604;
    case 3:
        uVar19 = 0;
        if ((MMIO32(DAT_00005954 + 0x10) & 0x40000000) != 0) uVar19 = DAT_00005958;
        uVar19 = uVar19 / ((MMIO32(0x40000388) & 0xff) + 1);
        break;
    case 4:
        uVar19 = Adc_ReadCh_LPO1MHz();
        break;
    case 6:
        uVar19 = GetClock_32k();
        break;
    default:
        goto switchD_caseD_5;
    }
    if (uVar19 == 0) {
switchD_caseD_5:
        FUN_00003eac(DAT_00005664, 0x4a, 0);
    } else {
LAB_00005604:
        iVar15 = FUN_000022b0(DAT_00005690, 2);
        if ((iVar15 != 0) ||
            (iVar15 = func_0x00002754(DAT_00005690, &local_3c, uVar19), iVar15 != 0) ||
            (iVar14 = FUN_000078f0(DAT_00005690, DAT_00005694,
                                   *(uint32_t *)(iVar14 + 0x134)), iVar14 != 0)) {
            goto switchD_caseD_5;
        }
        switch (MMIO32(0x400002c0)) {                /* SAI bit-clock divider */
        case 0:
            uVar19 = GetSystemCoreClockSource();
            break;
        case 1:
            uVar19 = *DAT_0000595c / ((MMIO32(0x400003c4) & 0xff) + 1);
            break;
        case 2:
            uVar19 = 0;
            if ((MMIO32(DAT_00005954 + 0x10) & 0x4000) != 0) uVar19 = DAT_000059a8;
            break;
        case 3:
            uVar19 = 0;
            if ((MMIO32(DAT_00005954 + 0x10) & 0x40000000) != 0) uVar19 = DAT_00005958;
            uVar19 = uVar19 / ((MMIO32(0x40000388) & 0xff) + 1);
            break;
        case 4:
            uVar19 = Adc_ReadCh_LPO1MHz();
            break;
        case 6:
            uVar19 = GetClock_32k();
            break;
        default:
            uVar19 = 0;
            break;
        }
        uVar28 = (MMIO32(0x40000330) & 0xff00) / ((MMIO32(0x40000330) & 0xff) + 1) + 1;
        if ((uVar19 < uVar28) ||
            (iVar14 = FUN_000022b0(DAT_00005960, 2), iVar14 != 0) ||
            (iVar14 = func_0x00002754(DAT_00005960, &local_3c, uVar19 / uVar28), iVar14 != 0) ||
            (uVar19 = FUN_000078f0(DAT_00005960, DAT_00005964,
                                   *(uint32_t *)(iVar16 + 0x134)), uVar19 != 0) ||
            (iVar16 = xTaskCreate(DAT_00005970, DAT_0000596c, 0x186,
                                  (void *)(intptr_t)DAT_00005968, 3, NULL), iVar16 != 1) || /* l_led_ring_task */
            (iVar16 = xTaskCreate(DAT_00005978, DAT_00005974, 0x186,
                                  (void *)(intptr_t)DAT_00005968, 3, NULL), iVar16 != 1)) { /* r_led_ring_task */
            goto switchD_caseD_5;
        }
        /* registry_add device sweep, probe, frames */
        local_3c = uVar19;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 0x2c;
        local_30 = DAT_0000597c;
        local_28 = DAT_00005980;
        local_24 = uVar17;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = uVar19; local_1c = uVar19; local_18 = uVar19;
        uStack_14 = (uint32_t)FUN_0000879c();
        if ((local_38 == 0) || (uStack_14 == 0) ||
            (uVar19 = registry_add(DAT_00005968, &local_3c), uVar19 != 0)) goto switchD_caseD_5;
        local_3c = uVar19;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 0xe;
        local_30 = DAT_00005984;
        local_28 = DAT_00005988;
        local_24 = uVar17;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = uVar19; local_1c = uVar19; local_18 = uVar19;
        uStack_14 = (uint32_t)FUN_0000879c();
        uVar19 = DAT_00005968;
        if ((local_38 == 0) || (uStack_14 == 0)) goto switchD_caseD_5;
        uVar28 = registry_add(DAT_00005968, &local_3c);
        if (uVar28 != 0) goto switchD_caseD_5;
        local_3c = uVar28;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 1;
        local_30 = DAT_0000598c;
        local_28 = DAT_00005990;
        local_24 = uVar19;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = uVar28; local_1c = uVar28; local_18 = uVar28;
        uStack_14 = (uint32_t)FUN_0000879c();
        if ((local_38 == 0) || (uStack_14 == 0) ||
            (uVar28 = registry_add(uVar19, &local_3c), uVar28 != 0)) goto switchD_caseD_5;
        local_3c = uVar28;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_30 = DAT_00005994;
        local_34 = 1;
        local_28 = DAT_00005998;
        local_24 = uVar19;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = uVar28; local_1c = uVar28; local_18 = uVar28; uStack_14 = uVar28;
        if ((local_38 == 0) || (uVar28 = registry_add(uVar19, &local_3c), uVar28 != 0))
            goto switchD_caseD_5;
        local_3c = uVar28;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 1;
        local_30 = DAT_0000599c;
        local_28 = DAT_000059a0;
        local_24 = uVar19;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = uVar28; local_1c = uVar28; local_18 = uVar28;
        uStack_14 = (uint32_t)FUN_0000879c();
        if ((local_38 == 0) || (uStack_14 == 0) ||
            (iVar16 = registry_add(uVar19, &local_3c), iVar16 != 0)) goto switchD_caseD_5;
        iVar16 = (int)FUN_00006a10(0x3c);
        piVar13 = DAT_000059ac;
        {
            char *pcVar8 = DAT_000059a4;
            if (iVar16 == 0) {
                *DAT_000059ac = 0;
LAB_000058c2:
                *pcVar8 = '\0';
LAB_000058c6:
                local_30 = (local_30 & 0xffff0000) | 0x90;
                local_34 = (local_34 & 0xffff0000)
                           | ((uint16_t)(((local_34 >> 8) & 0xff) | 1) << 8);
                local_34 = (local_34 & 0x0000ffff) | (0x6d1u << 16);
                local_3c = (local_3c & 0x00ffffff) | (0xd1u << 24);
                local_38 = (local_38 & 0xffffff00) | 6;
            } else {
                iVar14 = FUN_0000289c(iVar16);
                if (iVar14 != 0) {
                    FUN_000087f2((void *)iVar16);
                    *piVar13 = 0;
                    goto LAB_000058c2;
                }
                FUN_00003eac(DAT_00005c90, 0xd0, 3, 0x37400, 6, 3);
                FUN_00003eac(DAT_00005c90, 0xd3, 3, 0, 0x37400, (int)DAT_00005c94);
                FUN_00003eac(DAT_00005c90, 0xd3, 3, 1, 0x37600, 0x37c00);
                FUN_00003eac(DAT_00005c90, 0xd3, 3, 2, 0x37800, (int)DAT_00005ce4);
                *piVar13 = iVar16;
                iVar16 = func_0x00006708(iVar16, &local_3c);
                if (iVar16 != 0) {
                    FUN_00003eac(DAT_00005c98, 0x40, 0);
                    *pcVar8 = '\0';
                    goto LAB_000058c6;
                }
                iVar16 = func_0x00009876(DAT_00005c9c, &local_3c, 6);
                if (iVar16 == 0) {
                    if (((local_38 >> 16) & 0xff) < 2) {
                        *pcVar8 = (char)((local_38 >> 24) & 0xff);
                        if (((local_38 >> 24) & 0xff) != 0) goto LAB_00005a62;
                    } else {
                        FUN_00003eac(DAT_00005c98, 0x4a, 2,
                                     (int)((local_38 >> 16) & 0xff), 1, (int)DAT_00005ce4);
                        *pcVar8 = '\0';
                    }
                    goto LAB_000058c6;
                }
                FUN_00003eac(DAT_00005c98, 0x45, 5, (int)(local_3c & 0xff),
                             (int)((local_3c >> 8) & 0xff), (int)((local_3c >> 0x10) & 0xff),
                             (int)(local_3c >> 0x18), (int)(local_38 & 0xff));
                *pcVar8 = '\x01';
LAB_00005a62:
                local_30 = (local_30 & 0xffff0000) | 0xb4;
                local_34 = (local_34 & 0xffff0000)
                           | ((uint16_t)(((local_34 >> 8) & 0xff) | 1) << 8);
                local_34 = (local_34 & 0x0000ffff) | (0x3fdu << 16);
                local_3c = (local_3c & 0x00ffffff) | (0xfdu << 24);
                local_38 = (local_38 & 0xffffff00) | 3;
            }
        }
        local_3c = (local_3c & 0x0000ffff) | (((local_3c >> 16) & 0xffff) << 16) | 0x10000;
        func_0x00001884(0, 0, &local_3c, 0xe);
    }

    /* -------------------------------------------------------------------
     * 0x5b00 — DMIC/I2S peripheral + LPI2C(?) device + audio task
     * ----------------------------------------------------------------- */
    port_clock_wait(DAT_00005ca0);
    {
        uint32_t *puVar18d = DAT_00005ca4;
        MMIO8((int)DAT_00005ca4 + 0x30f) = 0x40;
        *puVar18d = 0x8000;
    }
    vmem_set(&local_3c, 0, 0xc);
    piVar13 = DAT_00005cac;
    local_38 = DAT_00005ca8;
    local_34 = (local_34 & 0xffff0000) | 0x2300;
    local_3c = (local_3c & 0xffffff00) | 1;          /* DMIC config */
    vmem_set(DAT_00005cac, 0, 0x50);
    iVar16 = FUN_0000974e();
    piVar13[0x12] = iVar16;
    if (iVar16 == 0) {
LAB_00005b10:
        FUN_00003eac(DAT_00005cb0, 0xf8, 0);
LAB_00005c7a:
        FUN_00003eac(DAT_00005ce0, 0x4f, 0);
    } else {
        iVar14 = (int)FUN_000095be(1, 0);
        piVar13[0x13] = iVar14;
        iVar16 = DAT_00005cb4;
        if (iVar14 == 0) {
            FUN_00003338(piVar13[0x12]);
            goto LAB_00005b10;
        }
        *piVar13 = DAT_00005cb4;
        FUN_000022b0(iVar16, 3);
        if ((local_3c & 0xff) == 0) {
            uVar19 = MMIO32(iVar16 + 0x800) & 0x1e;
        } else {
            uVar19 = (MMIO32(iVar16 + 0x800) & 0x1e) | 1;
        }
        MMIO32(iVar16 + 0x800) = uVar19;
        iVar16 = DAT_00005cb4;
        if (DAT_00005cb8 / local_38 < 4) {
            uVar19 = DAT_00005cbc / local_38;
        } else {
            uVar19 = 6;
        }
        uVar31 = local_38 * uVar19;
        uVar21 = 0; uVar30 = 0; uVar28 = 0;
        for (; uVar19 < 0x10001; uVar19 = uVar19 + 1) {  /* DMIC divider best-fit */
            uVar33 = DAT_00005ce8 / uVar31 + 5;
            uVar22 = uVar33 / 10;
            if (0x11 < uVar22) uVar22 = 0x12;
            uVar34 = DAT_00005cc0 - uVar31 * uVar22;
            uVar27 = uVar19;
            uVar29 = uVar34;
            if ((uVar30 <= uVar34) && (uVar30 != 0)) {
                uVar22 = uVar28;
                uVar27 = uVar21;
                uVar29 = uVar30;
            }
            uVar28 = uVar22;
            uVar21 = uVar27;
            if ((uVar34 == 0) || (uVar31 = uVar31 + local_38, uVar33 < 0x32)) break;
            uVar30 = uVar29;
        }
        MMIO32(DAT_00005cb4 + 0x814) = (uVar21 - 1) & 0xffff; /* decimation */
        uVar19 = uVar28 >> 1;
        uVar21 = uVar19 - 2;
        bVar35 = (int)(uVar28 << 0x1f) < 0;
        if (bVar35) uVar19 = uVar19 - 1;
        uVar28 = uVar21 * 0x10;
        if (bVar35) {
            uVar19 = uVar19 & 7;
        } else {
            uVar21 = uVar21 & 7;
        }
        uVar28 = uVar28 & 0x70;
        if (bVar35) {
            MMIO32(iVar16 + 0x824) = uVar19 | uVar28;  /* best-fit OSR */
        } else {
            MMIO32(iVar16 + 0x824) = uVar21 | uVar28;
        }
        uVar20 = DAT_00005cc4;
        iVar16 = DAT_00005cb4;
        uVar19 = (DAT_00005cc0 * ((local_34 >> 8) & 0xff)) / 0x640 + 5;
        if (uVar19 < 0xa00a) {
            uVar19 = uVar19 / 10;
        } else {
            uVar19 = 0x1000;
        }
        MMIO32(DAT_00005cb4 + 0x810) = (uVar19 - 1) * 0x10 | 0xf;
        vmem_set((void *)uVar20, 0, 0x40);
        piVar13[0xf] = (int)DAT_00005cc8;
        piVar13[0x10] = (int)piVar13;
        iVar14 = irqn_to_gpio_index(iVar16);
        *(uint32_t *)(DAT_00005ccc + iVar14 * 4) = uVar20;
        *(uint32_t *)(DAT_00005cd4 + iVar14 * 4) = DAT_00005cd0;
        *(uint32_t *)(iVar16 + 0x80c) = DAT_00005cd8;
        nvic_irq_enable(0xf);
        iVar16 = FUN_0000974e();
        *DAT_00005cdc = iVar16;
        if ((iVar16 == 0) ||
            (uVar19 = xTaskCreate(DAT_00006000, (const char *)(intptr_t)DAT_00005ff8, 0xb4,
                                  (void *)(intptr_t)DAT_00005ffc, 1, NULL), uVar19 != 1)) { /* audio/DMIC task */
            goto LAB_00005c7a;
        }
        local_3c = (uint32_t)FUN_00006a10(10);
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_30 = DAT_00006004;
        local_28 = 0; local_24 = 0;
        local_34 = 10;
        local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
        local_2c = (local_2c & 0xffffff00) | 2;
        if ((local_38 == 0) || (local_3c == 0) ||
            (uVar28 = registry_add(DAT_00005ffc, &local_3c), uVar28 != 0)) goto LAB_00005c7a;
        local_3c = (uint32_t)FUN_00006a10(2);
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 2;
        local_30 = DAT_00006008;
        local_2c = (local_2c & 0xffffff00) | 2;
        local_28 = uVar28; local_24 = uVar28; local_20 = uVar28;
        local_1c = uVar28; local_18 = uVar28; uStack_14 = uVar28;
        if ((local_38 == 0) || (local_3c == 0) ||
            (uVar28 = registry_add(DAT_00005ffc, &local_3c), uVar28 != 0)) goto LAB_00005c7a;
        local_3c = uVar28;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_30 = DAT_0000600c;
        local_28 = DAT_00006010;
        local_24 = uVar17;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_34 = uVar19;
        local_20 = uVar28; local_1c = uVar28; local_18 = uVar28; uStack_14 = uVar28;
        if ((local_38 == 0) ||
            (iVar16 = registry_add(DAT_00005ffc, &local_3c), iVar16 != 0)) goto LAB_00005c7a;
    }

    /* -------------------------------------------------------------------
     * 0x5fba — FlexCAN/comm port + dmic_task + comm interface bring-up
     * ----------------------------------------------------------------- */
    clock_div_program(0x120);
    iVar16 = DAT_00006070;
    piVar18 = DAT_00006014;
    MMIO32(0x400003ac) = 0;
    MMIO32(0x40000420) = 1;
    port_clock_wait(0x14);
    port_clock_wait(DAT_00006018);
    piVar18[0x60] = 0x20000;
    iVar14 = irqn_to_gpio_index(iVar16);
    *(uint32_t *)(DAT_0000601c + iVar14 * 4) = 0;
    *(uint32_t *)(DAT_00006024 + iVar14 * 4) = DAT_00006020;
    *piVar18 = 0x20000;
    pbVar3 = DAT_00006028;                              /* CAN bitrate descriptor */
    pbVar3[0] = 3;
    pbVar3[1] = 0;
    *(int16_t *)(pbVar3 + 6) = (int16_t)(*DAT_0000602c / 0xfa000);
    pbVar3[8] = 0;
    pbVar3[9] = 0x20;
    pbVar3[2] = 0;
    pbVar3[3] = 0;
    pbVar3[4] = 0;
    pbVar3[5] = 0;
    pbVar3[0xe] = 4;
    pbVar3[0xf] = 0;
    pbVar3[10] = 0x40;
    pbVar3[0xb] = 0;
    pbVar3[0xc] = 0;
    pbVar3[0xd] = 0;
    pbVar3[0x10] = 0;
    FUN_000022b0(iVar16, 5);
    iVar14 = DAT_00006074;
    uVar19 = DAT_00006030 & ((uint32_t) *(uint16_t *)(pbVar3 + 0xc) << 0x10);
    {
        uint16_t uVar1 = *(uint16_t *)(pbVar3 + 10);
        uint16_t uVar2;
        MMIO32(iVar16 + 0xc00) =                        /* CAN CTRL1 timing fields */
            ((uint32_t)pbVar3[3] << 9) | ((uint32_t)pbVar3[2] << 8)
            | ((uint32_t)pbVar3[4] << 0xc) | ((uint32_t)pbVar3[5] << 0xd)
            | ((uint32_t)pbVar3[8] << 10) | ((uint32_t)(pbVar3[0] & 3) << 4)
            | (uint32_t)(uint8_t)(pbVar3[1] << 6)
            | ((((uint32_t)pbVar3[9] - 1) * 0x10000) & 0x1f0000);
        uVar2 = *(uint16_t *)(pbVar3 + 6);
        MMIO32(iVar16 + 0xc04) = (((uint32_t)uVar1 - 1) & 0x7ff) | uVar19;
        MMIO32(iVar16 + 0xc1c) = ((uint32_t)uVar2 - 1) & 0xfff;
    }
    puVar7 = DAT_00006034;
    bVar10 = pbVar3[0xe];
    MMIO32(iVar16 + 0xe00) = ((uint32_t)pbVar3[0x10] << 3) | 0x20002;
    MMIO32(iVar16 + 0xe08) = ((uint32_t)(bVar10 & 0xf) << 0x10) | 2;
    iVar15 = gpio_base_to_bank((uint32_t)(intptr_t)puVar7);
    pcc_gate_enable(*(uint16_t *)(DAT_00006038 + iVar15 * 2));
    port_clock_wait(*(uint32_t *)(DAT_0000603c + iVar15 * 4));
    puVar7[2] = *(uint32_t *)(DAT_00006040 + iVar15 * 4);
    *puVar7 = *puVar7 | 1;
    vmem_set((void *)iVar14, 0, 0x10);
    *(int *)(DAT_00006044 + (iVar15 * 0x17 + 8) * 4) = iVar14;
    iVar16 = DAT_00006048;
    *(uint32_t **)(iVar14 + 8) = puVar7;
    cVar11 = *(char *)(iVar16 + iVar15);
    MMIO8(iVar14 + 0xc) = 8;
    nvic_irq_enable((int)cVar11);
    MMIO32(*(int *)(iVar14 + 8) + 0x48) |= 0x100;
    puVar7[8] |= 0x100;
    puVar7[0x120] = (puVar7[0x120] & 0xfff8ffff) | 0x20000;
    *DAT_0000604c = (uint32_t)(intptr_t)FUN_000095be(1, 4);
    uVar19 = xTaskCreate(DAT_00006054, DAT_00006050, 0x5a,
                         (void *)(intptr_t)DAT_00005ffc, 4, NULL); /* dmic_task wrapper, prio 4 */
    if (uVar19 == 1) {
        local_3c = 0;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_30 = DAT_00006058;
        local_20 = 0; local_1c = 0;
        local_28 = DAT_0000605c;
        local_24 = uVar17;
        local_18 = 0; uStack_14 = 0;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_34 = uVar19;
        if ((local_38 == 0) ||
            (uVar19 = registry_add(DAT_00005ffc, &local_3c), uVar19 != 0)) goto LAB_00005fba;
        local_3c = (uint32_t)FUN_00006a10(8);
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 8;
        local_30 = DAT_00006060;
        local_2c = (local_2c & 0xffffff00) | 2;
        local_28 = uVar19; local_24 = uVar19; local_20 = uVar19;
        local_1c = uVar19; local_18 = uVar19; uStack_14 = uVar19;
        if ((local_38 == 0) || (local_3c == 0) ||
            (iVar16 = registry_add(DAT_00005ffc, &local_3c), iVar16 != 0)) goto LAB_00005fba;
    } else {
LAB_00005fba:
        FUN_00003eac(DAT_00006064, 0x54, 0);
    }
    local_3c = (uint32_t)FUN_00006a10(1);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 1;
    local_30 = DAT_00006068;
    local_2c = (local_2c & 0xffffff00) | 2;
    local_20 = 0; local_1c = 0;
    local_28 = DAT_0000606c;
    local_24 = uVar17;
    local_18 = 0; uStack_14 = 0;
    if ((local_38 != 0) && (local_3c != 0) &&
        (local_20 = (uint32_t)FUN_00006a10(0xc), local_20 != 0) &&
        (iVar16 = FUN_000015e0(local_20, 1000, 0, DAT_00006284, DAT_00006280), iVar16 == 0) &&
        (iVar16 = registry_add(DAT_00006284, &local_3c), iVar16 == 0)) {
        local_3c = iVar16;
        local_38 = (uint32_t)thunk_FUN_0000974e();
        local_34 = 1;
        local_30 = DAT_00006288;
        local_28 = DAT_0000628c;
        local_24 = uVar17;
        local_2c = (local_2c & 0xffffff00) | 1;
        local_20 = iVar16; local_1c = iVar16; local_18 = iVar16;
        uStack_14 = (uint32_t)FUN_0000879c();
        if ((local_38 != 0) && (uStack_14 != 0) &&
            (iVar14 = registry_add(DAT_00006284, &local_3c), iVar16 = DAT_00006290, iVar14 == 0)) {
            MMIO8(DAT_00006290 + 4) = 0;
            MMIO8(iVar16 + 0x17) = 0;
            goto LAB_000060f4;
        }
    }
    FUN_00003eac(DAT_000062b0, 0x59, 0);

LAB_000060f4:
    /* -------------------------------------------------------------------
     * 0x60f4 — Final device descriptor writes + sensors/control xTaskCreate
     * ----------------------------------------------------------------- */
    puVar7 = DAT_00006294;
    *DAT_00006294 = uVar17;
    puVar7[2] = 0xe0;
    puVar7[3] = 1;
    puVar7[5] = DAT_00006298;
    puVar7[4] = 0;
    puVar7[6] = DAT_0000629c;
    puVar7[1] = 2;
    puVar7[7] = DAT_000062a0;
    local_3c = (uint32_t)FUN_00006a10(8);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_28 = 0; local_24 = 0;
    local_34 = 8;
    local_30 = DAT_000062a4;
    local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
    local_2c = (local_2c & 0xffffff00) | 2;
    if ((local_38 == 0) || (local_3c == 0)) {
        cVar11 = -1;
    } else {
        cVar11 = (char)registry_add(DAT_00006284, &local_3c);
    }
    uVar17 = *puVar7;
    local_3c = (uint32_t)FUN_00006a10(1);
    local_38 = (uint32_t)thunk_FUN_0000974e();
    local_34 = 1;
    local_30 = DAT_000062a8;
    local_2c = (local_2c & 0xffffff00) | 2;
    local_28 = DAT_000062ac;
    local_24 = (uint32_t)(intptr_t)puVar7;
    local_20 = 0; local_1c = 0; local_18 = 0; uStack_14 = 0;
    if ((local_38 == 0) || (local_3c == 0) ||
        (cVar12 = (char)registry_add(uVar17, &local_3c), cVar12 != '\0') || cVar11 != '\0') {
        FUN_00003eac(DAT_000062b0, 0x65, 0);
    }
    FUN_00003eac(DAT_000062b0, 0x69, 0);
    iVar16 = xTaskCreate(DAT_000062bc, DAT_000062b8, 0x5a, NULL, 0, DAT_000062b4); /* comm/IOM task */
    if (iVar16 != 1) goto LAB_00006260;
    FUN_00006b44();
    if ((*DAT_000062c0 != 0) &&
        (iVar16 = xTaskCreate(DAT_000062cc, DAT_000062c8, 0x10e, NULL, 4, DAT_000062c4),
         iVar16 != 0)) {                            /* conditional control task */
        if (iVar16 != 1) goto LAB_00006260;

        /* ---------------------------------------------------------------
         * 0x6200 — SHPR3 + 1 ms SysTick + scheduler start (no return)
         * ------------------------------------------------------------- */
        vPortRaiseBASEPRI();                        /* 0x6200 */
        *DAT_000062d0 = 0xffffffff;
        *DAT_000062d4 = 1;
        *DAT_000062d8 = 0;
        MMIO32(0xe000ed20) |= 0xffff0000;           /* SHPR3: PendSV & SysTick prio 0xFF */
        MMIO32(0xe000e018) = 0;                     /* SYST_CVR = 0 */
        MMIO32(0xe000e014) = *DAT_000062dc / 1000 - 1; /* SYST_RVR = SystemCoreClock/1000 - 1 */
        MMIO32(0xe000e010) = 7;                     /* SYST_CSR = ENABLE|TICKINT|CLKSOURCE */
        *DAT_000062e0 = 0;
        vPortStartFirstTask_ControlTask();          /* 0x624c bl 0x370 — never returns */
        /* Unreachable post-scheduler tail Ghidra reconstructed (kept for fidelity). */
        vTaskSwitchContext_SelectNext();
        FUN_0000642c();
        do {
            FUN_00000820();
LAB_00006260:
            iVar16 = iVar16 + 1;
        } while (iVar16 != 0);
        vPortRaiseBASEPRI();
        for (;;) { }
    }
    vPortRaiseBASEPRI();
    for (;;) { }
}
