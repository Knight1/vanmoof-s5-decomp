/*
 * foc.c -- VanMoof S5 motor_control (control ISRs (FOC loop)).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== pie_init  (OEM 0x082923, isr) =====
 * Initialises the F28004x PIE (Peripheral Interrupt Expansion) controller. Disables global interrupts (INTM, DBGM) and clears IER and IFR registers. Zeroes all PIE interrupt-enable registers (PIEIERx at 0xCE2..0xCF8) and PIE interrupt-flag registers (PIEIFRx at 0xCE3..0xCF9) — 24 locations total (groups 1..12). Then enables the PIE block by setting bit 0 of PIECTRL (0xCE0). Called once at startup be
 * Peripherals: PIE: PIECTRL @0xCE0 (bit0=ENPIE); PIEIERx @0xCE2..0xCF8 (12 words); PIEIFRx @0xCE3..0xCF9 (12 words); CPU IER register (cleared to 0); CPU IFR register (cleared to 0); INTM+DBGM disabled via SETC
 * [confidence: high] */
/* 0x082923 - initialise PIE interrupt controller */
void pie_init(void) {
    /* Disable global interrupts and debug */
    asm(" SETC INTM, DBGM");
    IER = 0x0000U;  /* disable all CPU interrupt lines */
    IFR = 0x0000U;  /* clear all pending CPU interrupts */

    /* Zero all PIE IER and IFR registers (groups 1..12) */
    /* PIEIERx: 0xCE2, 0xCE4, 0xCE6 ... 0xCF8 */
    /* PIEIFRx: 0xCE3, 0xCE5, 0xCE7 ... 0xCF9 */
    *(volatile uint16_t *)0x0CE2 = 0;  /* PIEIER1 */
    *(volatile uint16_t *)0x0CE4 = 0;  /* PIEIER2 */
    *(volatile uint16_t *)0x0CE6 = 0;  /* PIEIER3 */
    *(volatile uint16_t *)0x0CE8 = 0;  /* PIEIER4 */
    *(volatile uint16_t *)0x0CEA = 0;  /* PIEIER5 */
    *(volatile uint16_t *)0x0CEC = 0;  /* PIEIER6 */
    *(volatile uint16_t *)0x0CEE = 0;  /* PIEIER7 */
    *(volatile uint16_t *)0x0CF0 = 0;  /* PIEIER8 */
    *(volatile uint16_t *)0x0CF2 = 0;  /* PIEIER9 */
    *(volatile uint16_t *)0x0CF4 = 0;  /* PIEIER10 */
    *(volatile uint16_t *)0x0CF6 = 0;  /* PIEIER11 */
    *(volatile uint16_t *)0x0CF8 = 0;  /* PIEIER12 */
    *(volatile uint16_t *)0x0CE3 = 0;  /* PIEISR1  */
    *(volatile uint16_t *)0x0CE5 = 0;  /* PIEISR2  */
    *(volatile uint16_t *)0x0CE7 = 0;  /* PIEISR3  */
    *(volatile uint16_t *)0x0CE9 = 0;  /* PIEISR4  */
    *(volatile uint16_t *)0x0CEB = 0;  /* PIEISR5  */
    *(volatile uint16_t *)0x0CED = 0;  /* PIEISR6  */
    *(volatile uint16_t *)0x0CEF = 0;  /* PIEISR7  */
    *(volatile uint16_t *)0x0CF1 = 0;  /* PIEISR8  */
    *(volatile uint16_t *)0x0CF3 = 0;  /* PIEISR9  */
    *(volatile uint16_t *)0x0CF5 = 0;  /* PIEISR10 */
    *(volatile uint16_t *)0x0CF7 = 0;  /* PIEISR11 */
    *(volatile uint16_t *)0x0CF9 = 0;  /* PIEISR12 */

    /* Enable PIE block */
    *(volatile uint16_t *)0x0CE0 |= 1U; /* PIECTRL.ENPIE = 1 */
}


/* ===== interrupt_enable_save_intm  (OEM 0x082B71, isr) =====
 * Enables one CPU or PIE interrupt while preserving and returning the previous INTM (global interrupt mask) state. Disables interrupts on entry (SETC INTM). If AH is in range 13..31, sets the corresponding CPU IER bit (IER |= 1<<(AH-1)). If AH>=32 the interrupt is PIE-level: derives the PIE group number from PH, reads PIExIER at 0x0CE2+group*2, OR-in the intra-group bit, and also sets the correspond
 * Peripherals: CPU IER (interrupt enable register); PIE group IERs at 0x0CE2..0x0CF8 (computed as (1649+group)*2); ST1 INTM/DBGM bits; CPU registers IER
 * [confidence: medium] */
/* 0x082B71 - interrupt_enable_save_intm */
uint16_t interrupt_enable_save_intm(uint32_t int_desc)
{
    /* int_desc: AH=interrupt_level(13..31) or group(>=32), AL=intra-group bit(1-based) */
    uint16_t old_intm;
    __asm(" push ST1");
    __asm(" setc INTM,DBGM");   /* global disable */
    __asm(" mov AL,*--SP");     /* pop old ST1 into AL */
    old_intm = (uint16_t)(int_desc & 1u); /* AR7 = INTM bit from saved ST1 */

    uint16_t level = (uint16_t)(int_desc >> 16u);
    if (level >= 32u) {
        /* PIE-level interrupt */
        uint16_t group = ((uint16_t)(int_desc >> 8u) & 0xFFu) - 1u;
        uint16_t bit_in_group = ((uint16_t)(int_desc & 0xFFu)) - 1u;
        uint16_t group_bit   = 1u << bit_in_group;
        /* PIExIER address: (1649 + group) * 2 = 0xCE2 + group*2 */
        volatile uint16_t *pie_ier = (volatile uint16_t *)((1649u + group) * 2u);
        *pie_ier |= group_bit;
        /* also enable the CPU-level IER for this group */
        uint16_t cpu_bit = 1u << group;
        IER |= cpu_bit;
    } else if (level >= 13u) {
        /* CPU IER bit */
        uint16_t ier_bit;
        if (level <= 16u) {
            ier_bit = 1u;
        } else {
            ier_bit = 0u;
        }
        ier_bit <<= (level - 1u);
        IER |= ier_bit;
    }

    /* restore INTM if it was enabled before */
    if (old_intm != 1u) {
        __asm(" push ST1");
        __asm(" clrc INTM,DBGM");
        __asm(" mov AL,*--SP");
    }
    return old_intm;   /* 0=was enabled, 1=was disabled */
}


/* ===== event_queue_dispatch  (OEM 0x082C8B, isr) =====
 * Walks a statically-allocated event queue stored in flash/data at 0x831A8..0x831C0. Compares head pointer (0x8326E) to tail pointer (0x83274) to detect a non-empty queue. For each pending entry dequeues a (type, args) pair: looks up the handler function pointer from a dispatch table rooted at 0x83268 indexed by the type byte, then calls it via XAR7 with the args struct in XAR5. Decrements entry cou
 * Peripherals: Flash/RAM event queue 0x83268..0x83280; dispatch vtable at 0x83268; entry pairs: (type_ptr, args_ptr); completion hook nullsub_1=0x831DC
 * [confidence: low] */
/* 0x082C8B - event_queue_dispatch */
extern void nullsub_1(void); /* 0x831DC - completion hook (empty) */

typedef void (*handler_fn)(void *args);

void event_queue_dispatch(void)
{
#define EVQ_BASE    ((uint32_t *)0x83268u)  /* dispatch table base (flash) */
#define EVQ_HEAD    ((uint32_t *)0x8326Eu)  /* queue head pointer */
#define EVQ_TAIL    ((uint32_t *)0x83274u)  /* queue tail / write pointer */
#define EVQ_END     ((uint32_t *)0x83280u)  /* queue end sentinel */

    uint32_t head_val = 0x8326Eu;
    uint32_t tail_val = *EVQ_TAIL;

    if (head_val == *EVQ_BASE)   /* empty check: base == stored sentinel */
        goto done;
    if (tail_val == *EVQ_END)
        goto done;

    /* compute entry count: (tail - head) / 4 - 1 */
    int32_t count = (int32_t)((tail_val - (uint32_t)EVQ_HEAD) >> 2u) - 1;

    uint32_t *p = EVQ_TAIL;
    do {
        uint32_t *item_ptr = (uint32_t *)*p++;        /* dequeue item pointer */
        uint8_t   type     = *(uint8_t *)item_ptr;   /* type byte at item[0] */
        uint32_t  tbl_off  = (uint32_t)type << 1u;   /* 2 words per entry */
        handler_fn *vtable = (handler_fn *)((uint32_t)EVQ_BASE + tbl_off);
        void *args         = (void *)*p++;            /* argument struct pointer */
        (*vtable)(args);                              /* dispatch */
        count--;
    } while (count != (int32_t)-1u);

done:
    nullsub_1();  /* completion hook */
}


/* ===== fault_dispatcher  (OEM 0x082CE3, isr) =====
 * Error/fault chain dispatcher. Reads error flag byte at 0xC0CA; if all bits set (0xFFFF == 0xFFFF), calls the ROM error trap at 0x3FFFFF. Then invokes the mandatory fault handler stored at *0xC0D6. If a secondary handler is registered at *0xC0CE, calls it with AL = the original error argument. If a tertiary handler is registered at *0xC0CC, calls it. Finally calls spin_forever to halt. One caller.
 * Peripherals: Fault flag byte 0xC0CA; handler pointers 0xC0D6 (mandatory), 0xC0CE (secondary), 0xC0CC (tertiary); ROM error vector 0x3FFFFF
 * [confidence: medium] */
/* 0x082CE3 - fault_dispatcher */
extern void spin_forever(void);            /* sub_82CE1 */
extern void rom_error_trap(void);          /* 0x3FFFFF - TI ROM __error() */

extern volatile uint8_t   g_fault_flags;   /* 0xC0CA */
extern volatile uint32_t  g_fault_handler; /* 0xC0D6 - mandatory fn ptr */
extern volatile uint32_t  g_fault_cb2;    /* 0xC0CE - optional fn ptr */
extern volatile uint32_t  g_fault_cb3;    /* 0xC0CC - optional fn ptr */

void fault_dispatcher(uint16_t fault_arg)
{
    if (g_fault_flags != 0u) {
        /* check if all error bits saturated -> call ROM trap */
        if ((uint32_t)g_fault_flags == 0xFFFFFFFFu)
            rom_error_trap();
    }

    /* call mandatory fault handler (fn ptr at 0xC0D6) */
    void (*handler)(void) = (void (*)(void))g_fault_handler;
    handler();

    /* optional secondary handler: receives the fault argument */
    if (g_fault_cb2 != 0u) {
        void (*cb2)(uint16_t) = (void (*)(uint16_t))g_fault_cb2;
        cb2(fault_arg);
    }

    /* optional tertiary handler (no argument) */
    if (g_fault_cb3 != 0u) {
        void (*cb3)(void) = (void (*)(void))g_fault_cb3;
        cb3();
    }

    spin_forever();
}


/* ===== pie_vect_table_init  (OEM 0x083049, isr) =====
 * Initialises the PIE (Peripheral Interrupt Expansion) vector table under EALLOW. Fills 220 consecutive 32-bit vector slots starting at PIE vector address 0x0D06 with the default ISR handler at 0x831B3. Then installs two specific handlers: 0x831D8 at PIE vector 0x0D24 (PIE3.1) and 0x831D6 at PIE vector 0x0D26 (PIE3.2). Releases EALLOW after setup.
 * Peripherals: PIEVect table @ 0x0D00-0x0EFF: slots 0x0D06-0x0EBE filled with default ISR 0x831B3; PIE3.1 @ 0x0D24 = 0x831D8; PIE3.2 @ 0x0D26 = 0x831D6.
 * [confidence: high] */
/* 0x083049 - pie_vect_table_init */
extern void default_pie_isr(void); /* @ 0x0831B3 */
extern void pie3_isr_1(void);      /* @ 0x0831D8 */
extern void pie3_isr_2(void);      /* @ 0x0831D6 */

void pie_vect_table_init(void)
{
    EALLOW;
    /* fill 220 PIE vector slots starting at 0x0D06 with default handler */
    volatile uint32_t *vect = (volatile uint32_t *)0x0D06uL;
    for (uint16_t i = 220u; i > 0u; i--) {
        *vect = (uint32_t)default_pie_isr;
        vect = (volatile uint32_t *)((uint16_t)vect + 2u);
    }
    /* install specific handlers */
    *(volatile uint32_t *)0x0D24uL = (uint32_t)pie3_isr_1;
    *(volatile uint32_t *)0x0D26uL = (uint32_t)pie3_isr_2;
    EDIS;
}
