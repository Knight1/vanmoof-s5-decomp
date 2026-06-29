/*
 * pwm.c -- VanMoof S5 motor_control (EPWM commutation + eQEP position + CPU timers).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== epwm_sync_aq_update  (OEM 0x082BD5, pwm) =====
 * Checks whether a sync-event trigger has occurred (bits 7 and 5 of EPWM state word [5] at ptr from *0xC34C offset 26). If triggered: clears bit5 of state[1] (active flag), increments GSxRAM counter 0xC578, updates AQCTLA (offset+10, clear bit15 on XAR0/set bit15 on XAR6) and AQCTLB (offset+11, clear bit13 on XAR5/set bit13 on XAR4) across four cached EPWM channel pointers loaded from *XAR7. Always 
 * Peripherals: EPWM AQCTLA offset+10 (bit15), AQCTLB offset+11 (bit13); TBSTS offset+1 (bit5=SYNCI); event counters 0xC576/0xC578; EPWM state base *0xC34C
 * [confidence: medium] */
/* 0x082BD5 - epwm_sync_aq_update */
extern void gpio_output_mode_set(uint32_t epwm_ptr, uint16_t threshold); /* sub_82F79 */
extern volatile uint32_t g_sync_trigger_cnt;  /* 0xC576 */
extern volatile uint32_t g_sync_aq_cnt;       /* 0xC578 */

void epwm_sync_aq_update(void)
{
#define PWM_STATE_PTR  (*(uint32_t **)0xC34Cu)
    uint32_t *state = (uint32_t *)PWM_STATE_PTR;
    volatile uint16_t *epwm = (volatile uint16_t *)(state[26/2]); /* XAR7 = state[offset26] */

    g_sync_trigger_cnt++;  /* 0xC576 */

    if ((epwm[5] & 0x00A0u) != 0u) {  /* bits 5 and 7 of TBPRD word */
        epwm[1] &= 0xFFDFu;           /* clear bit5 of TBSTS (SYNCI cleared) */
        g_sync_aq_cnt++;               /* 0xC578 */

        /* load four EPWM channel base pointers from same source */
        volatile uint16_t *ch0 = (volatile uint16_t *)*epwm;
        volatile uint16_t *ch1 = (volatile uint16_t *)*epwm;
        volatile uint16_t *ch2 = (volatile uint16_t *)*epwm;
        volatile uint16_t *ch3 = (volatile uint16_t *)*epwm;

        epwm[1] |= 0x0020u;  /* set bit5 again (re-arm) */

        /* manipulate AQCTLA (offset +10) for ch0/ch1 */
        ch0[10] &= 0x7FFFu;  /* clear bit15 */
        ch1[10] |= 0x8000u;  /* set   bit15 */
        /* manipulate AQCTLB (offset +11) for ch2/ch3 */
        ch2[11] &= 0xDFFFu;  /* clear bit13 */
        ch3[11] |= 0x2000u;  /* set   bit13 */
    }

    uint16_t threshold = 16u;
    gpio_output_mode_set((uint32_t)(state[26/2]), threshold); /* sub_82F79 */
}


/* ===== epwm_clock_enable  (OEM 0x082C32, pwm) =====
 * EALLOW-protected EPWM/HRPWM clock-enable state machine. ACC on entry selects the state: 0 = full disable (clears ClkCfgReg bit3, delays 300 NOPs, clears bits 1:0); 0x10000 = partial (clear bit0 set bit1 of ClkCfg+0x32); 0x20000 = call sub_82D0A (EPWM clock gating off, path A); 0x30000 = call sub_82F95 (EPWM TX enable, path B). Both 0x20000 and 0x30000 also set bit1/clear bit0 of ClkCfg+0x32. One c
 * Peripherals: ClkCfgRegs: CLKDIVSEL=0x5D208 (bits 3,1,0); HRCLK_CTL=0x5D22E; HRCLK_SEL=0x5D232 (bits 1,0); 300-NOP delay sequence
 * [confidence: medium] */
/* 0x082C32 - epwm_clock_enable */
extern void epwm_clkgate_disable_a(void); /* sub_82D0A */
extern void epwm_clkgate_disable_b(void); /* sub_82F95 */

#define CLKCFG_CLKDIVSEL    ((volatile uint16_t *)0x5D208u)
#define CLKCFG_HRCLK_CTL    ((volatile uint16_t *)0x5D22Eu)
#define CLKCFG_HRCLK_SEL    ((volatile uint16_t *)0x5D232u)

void epwm_clock_enable(uint32_t state)
{
    EALLOW;
    if (state == 0u) {
        /* full disable: clear bit3 of CLKDIVSEL, long delay, clear bits 1:0 */
        *CLKCFG_CLKDIVSEL &= 0xFFF7u;
        for (volatile int i = 250; i; i--) __asm(" nop");
        for (volatile int i = 50;  i; i--) __asm(" nop");
        *CLKCFG_CLKDIVSEL &= 0xFFFCu;
    } else if (state == 0x10000u) {
        /* partial enable: clear bit0, set bit1 of HRCLK_SEL */
        uint16_t v = *CLKCFG_HRCLK_SEL & 0xFFFEu;
        v |= 2u;
        *CLKCFG_HRCLK_SEL = v;
    } else if (state == 0x20000u) {
        epwm_clkgate_disable_a();   /* sub_82D0A */
        uint16_t v = *CLKCFG_HRCLK_SEL & 0xFFFCu;
        v |= 1u;
        *CLKCFG_HRCLK_SEL = v;
    } else if (state == 0x30000u) {
        epwm_clkgate_disable_b();   /* sub_82F95 */
        uint16_t v = *CLKCFG_HRCLK_SEL & 0xFFFCu;
        v |= 1u;
        *CLKCFG_HRCLK_SEL = v;
    }
    EDIS;
}


/* ===== sw_timer_tick_dispatch  (OEM 0x082D32, timer) =====
 * Software timer tick: iterates channels 0-15. For each channel whose bit is set in the 16-bit active-channel bitmap at 0xC33E, decrements the 32-bit countdown counter in the table at 0xC652. When a counter reaches 0 and a non-null handler pointer exists in the table at 0xC548, calls the handler via indirect call. 1 caller.
 * Peripherals: GSxRAM: 0xC33E = 16-bit active-channel bitmap; 0xC652 = countdown counter table (16 x 32-bit entries); 0xC548 = callback handler pointer table (16 x 32-bit entries). No hardware peripherals.
 * [confidence: high] */
/* @0x082D32 - Software timer tick: decrement all active timers, fire expired callbacks */
typedef void (*sw_timer_cb_t)(void);

extern uint16_t sw_timer_active_bitmap;  /* 0xC33E */
extern uint32_t sw_timer_countdown[16];  /* 0xC652 */
extern sw_timer_cb_t sw_timer_handler[16]; /* 0xC548 */

void sw_timer_tick_dispatch(void)
{
    uint16_t i;
    uint32_t *countdown;
    sw_timer_cb_t *handler;

    for (i = 0; i < 16u; i++)
    {
        /* Check if channel i is active */
        if (!((1u << i) & sw_timer_active_bitmap))
            continue;

        countdown = &sw_timer_countdown[i];
        if (*countdown == 0u)
            continue;

        *countdown -= 1u;
        if (*countdown != 0u)
            continue;

        /* Countdown reached zero - fire callback if registered */
        handler = &sw_timer_handler[i];
        if (*((uint32_t *)handler) == 0u)
            continue;

        (*handler)();
    }
}


/* ===== epwm_force_output_and_write_compare  (OEM 0x082D5A, pwm) =====
 * Forces EPWM action-qualifier outputs and writes a compare value. Performs a 7-cycle NOP sync delay, then manipulates bit 13 (0x2000) in CMPB register fields of two nested EPWM descriptor pointers (set on one, clear on other). Then sets bits [14:13] (0x6000) in the CMPA register area of the resolved EPWM base. After a 31-cycle NOP, polls *+XAR4[2] bit 5 until set (hardware sync), writes AH (the com
 * Peripherals: EPWM peripheral registers accessed via double-indirection through XAR4 struct pointer: +10 = CMPA compare area (or CMPCTL), +11 = CMPB compare area; bit 13 (0x2000) = shadow mode select; bits 14:13 (0x6000) = load/force control; +2 bit 5 = sync/ready status; +8 = compare register write target. No di
 * [confidence: medium] */
/* @0x082D5A - Force EPWM action-qualifier outputs and write compare value
 * XAR4 = pointer to EPWM descriptor struct (double-pointer)
 * ACC  = packed operand: AH[10:0] = compare value (merged with AL selector)
 */
void epwm_force_output_and_write_compare(void *xar4_ptr, uint16_t packed_acc)
{
    uint16_t **pp  = (uint16_t **)xar4_ptr;
    uint16_t *base = *pp;          /* first-level deref */
    uint16_t *inner;
    uint16_t compare_val = packed_acc & 0x07FFu; /* AH[10:0] merged with AL */
    int nop_i;

    /* 7-cycle NOP sync */
    for (nop_i = 7; nop_i > 0; nop_i--) { __asm(" nop"); }

    /* Manipulate CMPB shadow-mode bit (bit 13) in the two levels */
    inner = *base;            /* double deref */
    inner[11] &= 0xDFFFu;    /* clear bit 13 in inner struct +11 */
    base[11]  |= 0x2000u;    /* set  bit 13 in outer struct +11 */

    /* Reload outer pointer then access CMPA/CMPB force bits */
    base  = *pp;             /* re-read outer ptr */
    inner = *base;           /* double deref again */
    inner[10] |= 0x6000u;   /* set bits [14:13] in +10 (CMPA force/load ctrl) TODO verify */
    inner[11] |= 0x2000u;   /* set bit 13 in +11 (CMPB ctrl) */

    /* 31-cycle NOP delay */
    for (nop_i = 31; nop_i > 0; nop_i--) { __asm(" nop"); }

    /* Poll *+XAR4[2] bit 5 until set (hardware ready) */
    while (!(base[2] & (1u << 5))) { __asm(" nop"); }

    /* Write compare value to CMPCTL/CMPA register (+8) */
    base[8] = compare_val;

    /* 639-cycle settling delay */
    for (nop_i = 639; nop_i > 0; nop_i--) { __asm(" nop"); }

    /* spm 0 - reset product shift mode */
    __asm(" spm 0");
}


/* ===== sw_timer_channel_clear  (OEM 0x082DCF, timer) =====
 * Searches channels 0-15 for a slot not yet marked in the armed bitmap at 0xC33D. When found (bit for channel not set and search mask matches), zeros out the corresponding countdown entry (at 0xC652) and handler entry (at 0xC548), then marks the channel as armed in 0xC33D. Returns the found channel index in AL. Used to initialise or clear a software timer channel. 5 callers.
 * Peripherals: GSxRAM: 0xC33D = 16-bit armed-channel bitmap; 0xC652 = countdown counter table; 0xC548 = callback handler table. No hardware peripherals.
 * [confidence: medium] */
/* @0x082DCF - Locate a free (not yet armed) software timer channel and clear it
 * Returns: AL = channel index found (0-15), or current loop count if not found
 */
extern uint16_t sw_timer_armed_bitmap;   /* 0xC33D */
extern uint32_t sw_timer_countdown[16]; /* 0xC652 */
extern uint32_t sw_timer_handler[16];   /* 0xC548 */

uint16_t sw_timer_channel_clear(void)
{
    uint16_t search_bit = 1u; /* rotating bit mask starting at bit 0 */
    uint16_t chan;

    for (chan = 0; chan < 16u; chan++)
    {
        if (sw_timer_armed_bitmap & search_bit)
        {
            /* Channel already armed - advance search bit and try next */
            search_bit <<= 1u;
            continue;
        }

        /* Found unarmed channel - clear its entries */
        /* The table offset for this channel = chan * 2 words (32-bit entries) */
        uint32_t *cnt_slot = (uint32_t *)((uint16_t *)&sw_timer_countdown[0] + chan * 2u);
        uint32_t *hdl_slot = (uint32_t *)((uint16_t *)&sw_timer_handler[0]   + chan * 2u);

        /* Mark as armed in bitmap */
        sw_timer_armed_bitmap |= (uint16_t)search_bit;

        /* Zero out slot entries */
        *cnt_slot = 0u;
        *hdl_slot = 0u;

        return chan; /* TODO verify: returns channel index or PL=250? */
    }

    return chan; /* not found case */
}


/* ===== timer_channel_register  (OEM 0x082FB1, timer) =====
 * Registers a software timer channel. Takes channel index (0-15) in AR5, a 32-bit period value in ACC, and a data/callback pointer in XAR4. Validates channel < 16. Stores the period in period_table[0xC652 + channel*2], stores data pointer in ptr_table[0xC548 + channel*2], sets the corresponding bit in the active-channel bitmask at 0xC33E. Returns 1 on success, 0 if channel >= 16. Called from 13 site
 * Peripherals: GSxRAM: period_table @ 0x0C652 (16 x 32-bit entries); ptr_table @ 0x0C548 (16 x 32-bit entries); active_mask @ 0x0C33E (16-bit bitmask).
 * [confidence: high] */
/* 0x082FB1 - timer_channel_register */
/* period_table at 0xC652, ptr_table at 0xC548, active_mask at 0xC33E */
#define TIMER_PERIOD_TABLE ((volatile uint32_t *)0x0C652uL)
#define TIMER_PTR_TABLE    ((volatile uint32_t *)0x0C548uL)
#define TIMER_ACTIVE_MASK  (*(volatile uint16_t *)0x0C33EuL)

uint16_t timer_channel_register(uint16_t channel /* AR5 */,
                                uint32_t period   /* ACC */,
                                void *data_ptr    /* XAR4 */)
{
    if (channel >= 16u)
        return 0u;

    TIMER_PERIOD_TABLE[channel] = period;
    TIMER_PTR_TABLE[channel]    = (uint32_t)data_ptr;
    TIMER_ACTIVE_MASK |= (uint16_t)(1u << channel);
    return 1u;
}


/* ===== eqep_write_config_regs  (OEM 0x083077, eqep) =====
 * Writes three configuration words to an eQEP (quadrature encoder) peripheral register block. The eQEP base address is read from object[62] (32-bit ptr member). Reads config word from object[30] and writes it to eQEP[+0] (QDECCTL). Then writes constant 29 (0x1D) to eQEP[+2] (QEPCTL) and constant 9 (0x09) to eQEP[+4] (QCAPCTL). The three sub-functions (sub_831D0/D2/D4) are simple register-store stubs
 * Peripherals: eQEP1 @ 0x5100, eQEP2 @ 0x5140. Registers: QDECCTL @ [base+0], QEPCTL @ [base+2], QCAPCTL @ [base+4]. Object ptr stored at [obj+62].
 * [confidence: medium] */
/* 0x083077 - eqep_write_config_regs */
/* sub_831D4: *+XAR4[0] = ACC (QDECCTL) */
/* sub_831D0: *+XAR4[2] = ACC (QEPCTL)  */
/* sub_831D2: *+XAR4[4] = ACC (QCAPCTL) */

typedef struct {
    /* ... other members ... */
    uint32_t config_word;  /* offset 30: decoder control config */
    /* ... */
    volatile uint16_t *eqep_base; /* offset 62: eQEP peripheral base ptr (32-bit) */
} EqepObj;

void eqep_write_config_regs(EqepObj *obj)
{
    volatile uint16_t *base = obj->eqep_base;    /* *+XAR2[62] */
    uint32_t cfg = *(uint32_t *)((uint16_t *)obj + 30); /* *+XAR2[30] */

    /* sub_831D4 */
    *(volatile uint32_t *)(base + 0) = cfg;     /* QDECCTL = config */
    /* sub_831D0 */
    *(volatile uint32_t *)(base + 2) = 29u;     /* QEPCTL  = 0x1D  */
    /* sub_831D2 */
    *(volatile uint32_t *)(base + 4) = 9u;      /* QCAPCTL = 0x09  */
}


/* ===== timer_channel_deactivate  (OEM 0x08308D, timer) =====
 * Searches a software-timer channel table (base 0x0C5EA, 8 slots, stride 13 words) for a slot whose channel-ID field (at slot[+2]) matches the input channel number (in AL). On finding a match, clears the slot's active flag (slot[+1] = 0). If no match found within 8 iterations, returns without modifying anything.
 * Peripherals: GSxRAM: timer channel table @ 0x0C5EA (8 x 13-word slots). Slot layout: [+0]=unused, [+1]=active_flag, [+2]=channel_id.
 * [confidence: medium] */
/* 0x08308D - timer_channel_deactivate */
#define CHAN_TABLE_BASE 0x0C5EAuL
#define CHAN_STRIDE     13u

typedef struct {
    uint16_t unused;      /* +0 */
    uint16_t active;      /* +1 */
    uint16_t channel_id;  /* +2 */
    uint16_t data[10];    /* +3..+12 */
} TimerSlot;

void timer_channel_deactivate(uint16_t channel /* AL */)
{
    TimerSlot *table = (TimerSlot *)CHAN_TABLE_BASE;
    for (uint16_t i = 0u; i < 8u; i++) {
        if ((uint32_t)table[i].channel_id == (uint32_t)channel) {
            table[i].active = 0u;
            return;
        }
    }
}


/* ===== timer_channel_is_free  (OEM 0x0830EF, timer) =====
 * Checks whether a software timer channel slot is free (handler/period == 0). Input channel index in AL; returns 1 if the 32-bit period entry in period_table[0xC652][channel] is zero (slot free), 0 if occupied or if channel >= 16. Called from 6 sites, used before allocating a new channel.
 * Peripherals: GSxRAM: period_table @ 0x0C652 (16 x 32-bit entries); active_mask @ 0x0C33E (indirectly).
 * [confidence: high] */
/* 0x0830EF - timer_channel_is_free */
#define TIMER_PERIOD_TABLE ((volatile uint32_t *)0x0C652uL)

uint16_t timer_channel_is_free(uint16_t channel /* AL */)
{
    if (channel >= 16u)
        return 0u;
    return (TIMER_PERIOD_TABLE[channel] == 0u) ? 1u : 0u;
}


/* ===== epwm_tb_prd_mode_set  (OEM 0x083159, pwm) =====
 * Writes a 2-bit field into bits[15:14] of a peripheral register word (pointed to by ACC). Clears bits[15:14] of *XAR5 then ORs in AR4 value shifted left 14 (from ACC<<14 before the OR). Used to set TBCTL.PRDLD or a similar 2-bit mode field in an ePWM time-base register struct. Called once from an ePWM initialization path.
 * Peripherals: ePWM register word at *ACC (struct field at offset 0); mask 0x3FFF (clear bits[15:14]); AR4 = 2-bit field value (shifted left 14)
 * [confidence: medium] */
/* @0x083159 - epwm_tb_prd_mode_set */
/* ACC=ptr to reg word, AR4=2-bit field value; inserts field into bits[15:14] */
void epwm_tb_prd_mode_set(uint16_t *reg, uint16_t field_val) {
    uint16_t *p = reg;              /* movl XAR5,@ACC */
    uint16_t v = *p & 0x3FFFu;     /* and AL,*XAR5,#3FFFh : clear bits[15:14] */
    uint16_t ar6 = (uint16_t)v;    /* movz AR6,@AL */
    uint16_t shifted = field_val << 14; /* mov ACC,@AR4 << #14 */
    v = (shifted & 0xFFFF) | ar6;  /* or AL,@AR6 : merge */
    *p = v;                        /* mov *XAR5,AL */
}


/* ===== epwm_dbctl_field_write  (OEM 0x083182, pwm) =====
 * Writes a 2-bit field into bits[11:10] of the ePWM Dead-Band Control register (or similar word at *XAR5+4). ACC is the object pointer (XAR5 = *ACC); mask 0xF3FF clears bits[11:10]; then ORs in AR4. Called 3 times with XAR4=2048 (0x800, bit 11) to set DBCTL.IN_MODE or POLSEL fields. Used during ePWM dead-band configuration.
 * Peripherals: ePWM register word at (*ACC)[4] (offset 8 bytes); mask 0xF3FF = clear bits[11:10]; AR4 = field value (callers use 0x800 = bit11)
 * [confidence: medium] */
/* @0x083182 - epwm_dbctl_field_write */
/* ACC=ptr-to-obj, AR4=field_value; write field into bits[11:10] of obj[4] */
void epwm_dbctl_field_write(uint16_t *obj, uint16_t field_val) {
    uint16_t *p = (uint16_t *)obj;  /* movl XAR5,@ACC */
    uint16_t v = p[4] & 0xF3FFu;   /* and AL,*+XAR5[4],#0F3FFh */
    v |= field_val;                 /* or AL,@AR4 */
    p[4] = v;                       /* mov *+XAR5[4],AL */
}


/* ===== epwm_phase_idx_clamp6  (OEM 0x0831A7, pwm) =====
 * Validates that an ePWM phase/channel index (in AL) is >= 6. If AL < 6: clears XAR4 to null (invalid). If AL >= 6: leaves XAR4 unchanged (valid pointer passes through). Acts as a bounds-guard for a 6-phase ePWM resource (e.g. phases A/B/C with hi/lo = 6 half-bridges). Two callers, both passing AL=6.
 * Peripherals: None; purely a range guard for ePWM phase enum (threshold 6)
 * [confidence: medium] */
/* @0x0831A7 - epwm_phase_idx_clamp6 */
/* AL=enum_val, XAR4=ptr; returns XAR4=NULL if AL<6 */
void *epwm_phase_idx_clamp6(uint16_t idx, void *ptr) {
    if (idx < 6) return NULL; /* cmpb AL,#6; sb locret,GEQ; movb XAR4,#0 */
    return ptr;
}


/* ===== epwm_module_idx_clamp10  (OEM 0x0831AB, pwm) =====
 * Validates that an ePWM module index (in AL) is >= 10. If AL < 10: clears XAR4 to null (invalid). If AL >= 10: leaves XAR4 unchanged (valid pointer passes through). Guards a 10-entry ePWM module table (F280049C has ePWM1-8 plus CLA/HRPWM channels). Four callers in the main CAN/SCI receive dispatch loop.
 * Peripherals: None; range guard for ePWM module enum (threshold 10)
 * [confidence: medium] */
/* @0x0831AB - epwm_module_idx_clamp10 */
/* AL=enum_val, XAR4=ptr; returns XAR4=NULL if AL<10 */
void *epwm_module_idx_clamp10(uint16_t idx, void *ptr) {
    if (idx < 10) return NULL; /* cmpb AL,#10; sb locret,GEQ; movb XAR4,#0 */
    return ptr;
}
