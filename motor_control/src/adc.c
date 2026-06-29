/*
 * adc.c -- VanMoof S5 motor_control (ADC (phase-current) config + sampling).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== adc_soc_init  (OEM 0x081D65, adc) =====
 * Initialises three ADC modules (A, B, C) whose base addresses are read from a struct at XAR4: calls the common ADC-config helper sub_82814 for each, waits (~100 counts) via unk_E65A delay, clears ADC clock prescale in a shared register at 0x5D768, sets the ADCCTL2 PRESCALE field to SYSCLK/2 mode, sets ADCINTSEL (triggers SOC from EPWM), programs ADCSOC0CTL-ADCSOC7CTL with channel numbers and acquis
 * Peripherals: ADC struct ptr at XAR4; sub_82814 (ADC channel/trim helper); delay unk_E65A; ADC_TRIM_REG @0x5D768 (clears bits[10:8] and [2:0]); ADCA_BASE=0x7400, ADCB_BASE=0x7480, ADCC_BASE=0x7500; SOC registers: ADCA+0x10 (ADCSOC0CTL), ADCA+0x12, ADCA+0x14, ADCB+0x12, ADCB+0x14, ADCC+0x12, ADCC+0x14 etc. SOC val
 * [confidence: medium] */
/* 0x081D65 - ADC SOC and prescaler initialisation */
/* XAR4 -> struct { uint32_t *adcA; uint32_t *adcB; uint32_t *adcC; } */
void adc_soc_init(AdcConfig *cfg) {
    uint32_t *adcA = cfg->adcA;   /* XAR1[0] */
    uint32_t *adcB = cfg->adcB;   /* XAR1[2] */
    uint32_t *adcC = cfg->adcC;   /* XAR1[4] */

    delay_cycles(100);             /* unk_E65A(100) */
    adc_set_channel(adcC, 0, 0);   /* sub_82814: ADCC, XAR4=0, XAR5=0 */
    adc_set_channel(adcB, 0, 0);
    adc_set_channel(adcA, 0, 0);
    delay_cycles(100);

    /* Clear ADC clock prescale field in shared trim register @0x5D768 */
    EALLOW;
    HWREG(0x5D768) &= 0xF8FF;  /* clear bits[10:8] */
    HWREG(0x5D768) &= 0xFFF8;  /* clear bits[2:0]  */
    EDIS;

    /* Per-ADC: set ADCCTL1 prescale = /2 (ADCCTL2[3:0]=2) */
    EALLOW;
    adcA[1] = (adcA[1] & 0xFFF0) | 0x2;   /* ADCCTL2 */
    EDIS;
    EALLOW;
    adcB[1] = (adcB[1] & 0xFFF0) | 0x2;
    EDIS;
    EALLOW;
    adcC[1] = (adcC[1] & 0xFFF0) | 0x2;
    EDIS;

    /* Enable ADCs (ADCCTL1.ADCPWDNZ = 1, bit 2) */
    EALLOW; adcA[0] |= 4; EDIS;
    EALLOW; adcB[0] |= 4; EDIS;
    EALLOW; adcC[0] |= 4; EDIS;

    /* Set ADCBURSTCTL (bit 7) */
    EALLOW; adcA[0] |= 0x80; EDIS;
    EALLOW; adcB[0] |= 0x80; EDIS;
    EALLOW; adcC[0] |= 0x80; EDIS;

    /* ADCINTSEL1N2: trigger select offset 9 = 0x10 (ADCINTSEL) */
    EALLOW; adcA[9]  = (adcA[9]  & 0xFFF0) | 16; EDIS;
    EALLOW; adcB[9]  = (adcB[9]  & 0xFFF0) | 16; EDIS;
    EALLOW; adcC[9]  = (adcC[9]  & 0xFFF0) | 16; EDIS;

    delay_cycles(1000);

    /* ADCSOC0CTL for ADCB: base+7 */ 
    EALLOW;
    adcB[7] = (adcB[7] & 0xFFF0) | 2;  /* CHSEL or PRESCALE low nibble = 2 */
    EDIS;

    /* ADCSOC0CTL / ADCSOC1CTL etc - SOC channel and trigger programming */
    /* adcA SOC0CTL (base+16): AL=13 AH=82 -> 0x0052000D */
    EALLOW; *(uint32_t*)(&adcA[16]) = 0x0052000DUL; EDIS; /* CHSEL=13,TRIGSEL=EPWM1SOCA TODO verify */
    /* adcC SOC0CTL (base+16): 0x0052000D */
    EALLOW; *(uint32_t*)(&adcC[16]) = 0x0052000DUL; EDIS;
    /* adcB SOC0CTL (base+16): AL=13 AH=80 -> 0x0050000D */
    EALLOW; *(uint32_t*)(&adcB[16]) = 0x0050000DUL; EDIS; /* TODO verify TRIGSEL */
    /* adcA SOC1CTL (base+18): AL=13 AH=81 -> 0x0051000D */
    EALLOW; *(uint32_t*)(&adcA[18]) = 0x0051000DUL; EDIS;
    /* adcB SOC1CTL (base+18): 0x0051000D */
    EALLOW; *(uint32_t*)(&adcB[18]) = 0x0051000DUL; EDIS;
    /* adcC SOC1CTL (base+18): AL=13 AH=80 -> 0x0050000D */
    EALLOW; *(uint32_t*)(&adcC[18]) = 0x0050000DUL; EDIS;
    /* adcC SOC2CTL (base+20): AL=13 AH=81 */
    EALLOW; *(uint32_t*)(&adcC[20]) = 0x0051000DUL; EDIS;
    /* adcA SOC3CTL (base+22): AL=13 AH=83 -> 0x0083000D */
    EALLOW; *(uint32_t*)(&adcA[22]) = 0x0083000DUL; EDIS;
}


/* ===== adc_channel_select  (OEM 0x082814, adc) =====
 * Identifies which ADC module (A=1, B=2, C=0) from the input base address (0x7400/0x7480/0x7500). Reads an ADC calibration entry from OTP at 0x70594 + 6 × adc_index. Shifts the 16-bit OTP word right by 0 or 8 bits depending on XAR4/XAR5 signal flags. Stores the resulting 8-bit trim byte at *+XAR6[59]. Under EALLOW: sets or clears a bit in ADC_TRIM_REG (0x5D768) for the ADC-index position to connect 
 * Peripherals: ADCA_BASE @0x7400, ADCB_BASE @0x7480, ADCC_BASE @0x7500 (input identification); OTP ADC trim table @0x70594 (6 words/ADC: stride=6); ADC_TRIM_REG @0x5D768 (EALLOW protected, bit = 1<<adc_index); XAR5 (enable signal A), XAR4 (enable signal B); output byte at *+XAR6[59]
 * [confidence: medium] */
/* 0x082814 - select ADC channel and apply OTP calibration trim */
/* ACC = ADC base address; XAR4 = signal_B_enable; XAR5 = signal_A_enable */
/* XAR6 = output struct ptr (stores trim byte at [59]) */
void adc_channel_select(uint32_t adc_base, uint16_t sig_b, uint16_t sig_a, uint16_t *out) {
    uint16_t adc_idx;
    if      (adc_base == 0x7480UL) adc_idx = 1; /* ADCB */
    else if (adc_base == 0x7500UL) adc_idx = 2; /* ADCC */
    else                            adc_idx = 0; /* ADCA */

    uint16_t shift = (sig_a | sig_b) ? 0U : 8U;  /* shift OTP word */

    /* Read OTP trim for this ADC */
    EALLOW;
    uint32_t otp_addr = 0x70594UL + (uint32_t)6 * adc_idx;
    uint16_t otp_word = *(volatile uint16_t *)(otp_addr);
    uint8_t  trim     = (uint8_t)((otp_word >> shift) & 0xFFU);
    out[59]           = trim;  /* store at struct offset 59 */

    /* Configure ADC_TRIM_REG (0x5D768): set/clear signal-B bit */
    if (!sig_b) {
        uint16_t bit = (uint16_t)(1U << adc_idx);
        *(volatile uint16_t *)0x5D768 &= ~bit;
    } else {
        uint16_t bit = (uint16_t)(1U << adc_idx);
        *(volatile uint16_t *)0x5D768 |= bit;
    }

    /* Configure signal-A bit (offset by 256 from base bit) */
    if (!sig_a) {
        uint16_t bit = (uint16_t)(256U << adc_idx); /* TODO verify shift */ 
        *(volatile uint16_t *)0x5D768 &= ~bit;
    } else {
        uint16_t bit = (uint16_t)(256U << adc_idx);
        *(volatile uint16_t *)0x5D768 |= bit;
    }
    EDIS;
}


/* ===== adc_result_batch_process  (OEM 0x0828E1, adc) =====
 * Processes pending ADC conversion results from a result struct (XAR2/XAR5). Checks field [8] (new-data flag); if set, reads 6 ADC result words at offsets [2]..[7], applies a 11-bit mask (& 0x7FF = signed 11-bit ADC result range), pairs each with a channel-ID constant (4096, 6144, 8192, 10240, 12288, 14336, stepping in 2048), and calls sub_82D5A to scale/accumulate each sample. Clears field [8] (fla
 * Peripherals: ADC result struct in GS RAM: field[8]=new_data_flag, fields[2..7]=6 ADC samples (11-bit masked), fields[10],[12]=2 more samples; channel IDs: 0x1000,0x1800,0x2000,0x2800,0x3000,0x3800 (steps of 0x800); P register = output accumulator ptr; sub_82D5A (accumulate/scale sample)
 * [confidence: medium] */
/* 0x0828E1 - batch process ADC results for FOC current feedback */
/* XAR5 = AdcResultSet*; XAR4 = accumulator/output ptr (saved in P) */
void adc_result_batch_process(AdcResultSet *rs, FocAccum *acc) {
    if (rs->new_data_flag == 0)  /* field [8] */
        goto check_second;

    /* Process 6 phase current / voltage ADC channels */
    adc_sample_accumulate((rs->ch[0] & 0x7FFU), 0x1000U, acc); /* sub_82D5A: ch0, id=4096 */
    adc_sample_accumulate((rs->ch[1] & 0x7FFU), 0x1800U, acc); /* ch1, id=6144 */
    adc_sample_accumulate((rs->ch[2] & 0x7FFU), 0x2000U, acc); /* ch2, id=8192 */
    adc_sample_accumulate((rs->ch[3] & 0x7FFU), 0x2800U, acc); /* ch3, id=10240 */
    adc_sample_accumulate((rs->ch[4] & 0x7FFU), 0x3000U, acc); /* ch4, id=12288 */
    adc_sample_accumulate((rs->ch[5] & 0x7FFU), 0x3800U, acc); /* ch5, id=14336 */
    rs->new_data_flag = 0;  /* clear flag at [8] */

check_second:
    if (rs->extra_flag == 0)  /* field [14] */
        return;

    /* Process 2 additional samples (DC bus / temperature?) */
    /* field[10] shifted left 11 -> Q11 integer */
    uint32_t samp_a = (uint32_t)(rs->extra[0]) << 11;  /* field [10] << 11 */
    uint16_t samp_b_id = rs->extra[1];                  /* field [12], raw AH */
    adc_sample_accumulate((uint16_t)(samp_a >> 16), (uint16_t)samp_a, acc); /* sub_82D5A */
    adc_sample_accumulate(samp_b_id, 0, acc);
    rs->extra_flag = 0;  /* clear [14] */
}
