#ifndef USER_ECU_HAL_H
#define USER_ECU_HAL_H

#include <stdint.h>

/*
 * Small HAL helpers.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS).
 */

/*
 * Linear-search a 9-entry table (at OEM rodata 0x0000a510) for the supplied
 * key (a GPIO/PORT peripheral base, used by the firmware as an "IRQn") and
 * return its 0..8 index.
 *
 * The OEM returns the loop counter in r0; Ghidra renders the prototype as
 * void because it loses track of r0 across the early returns, but every
 * caller consumes the return value as a table index (see the IRQ
 * trampolines at 0x00002100 and the handler installer at 0x000078f0).
 * // 0x000020e4
 */
int irqn_to_gpio_index(int irqn);

/*
 * Return the 32 kHz clock frequency (32768 Hz) if the relevant SCG clock
 * source is valid and enabled, otherwise 0.
 *
 * Reads the System Clock Generator (SCG, base 0x40020000) status registers
 * as volatile MMIO. // 0x00001188
 */
uint32_t GetClock_32k(void);

#endif /* USER_ECU_HAL_H */
