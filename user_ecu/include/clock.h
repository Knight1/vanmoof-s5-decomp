#ifndef USER_ECU_CLOCK_H
#define USER_ECU_CLOCK_H

#include <stdint.h>

/*
 * Clock / peripheral glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS).
 *
 * Thin bare-metal accessors over the System Clock Generator (SCG) and the
 * peripheral clock-gate / NVIC. All register access is verbatim VanMoof
 * MMIO (volatile pointers at absolute addresses).
 */

/*
 * Resolve the active core-clock source frequency, in Hz.
 *
 * Decodes the clock-select registers at 0x40000284 / 0x40000280 plus the SCG
 * status bits, returning one of:
 *   12000000 (12 MHz fast IRC), 96000000 (96 MHz PLL),
 *   1000000  (1 MHz LPO, via Adc_ReadCh_LPO1MHz),
 *   the 32 kHz clock (via GetClock_32k),
 *   a runtime-computed value held in a RAM frequency global, or 0 if the
 *   selected source is not valid/enabled.
 * // 0x000011c0
 */
uint32_t GetSystemCoreClockSource(void);

/*
 * Return the 1 MHz LPO constant (1000000) when the SCG LPO clock is valid
 * (status reg 0x40000a18, bit 6 set), otherwise 0.
 * // 0x00001170
 */
uint32_t Adc_ReadCh_LPO1MHz(void);

/*
 * Program up to two packed clock-divider descriptors.
 *
 * `packed` holds two 12-bit fields ({selector[7:0], value[11:8]}, low field
 * first); the programmed divider is value-1. Selector 0x3f writes bit 0 of the
 * divider-config word at 0x40020098, selector 0x3e writes bits 5:4 of
 * 0x4002009c, and any other selector writes the byte to the divider-register
 * array word at 0x40000260 + selector*4. A zero field is skipped; processing
 * stops once the remaining bits are zero.
 * // 0x0000110c
 */
void clock_div_program(uint32_t packed);

/*
 * Enable a peripheral: ungate its clock (clock-gate controller @ 0x40004000,
 * gate index == param_1), then clear-pending and enable the associated NVIC
 * interrupt. The IRQ number is looked up from an OEM rodata table indexed by
 * the gate index; a negative entry means "no interrupt".
 * // 0x00007988
 */
void periph_clk_nvic_enable(int param_1);

#endif /* USER_ECU_CLOCK_H */
