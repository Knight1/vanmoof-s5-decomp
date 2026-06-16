#ifndef USER_ECU_SYSCLOCK_H
#define USER_ECU_SYSCLOCK_H

/*
 * sysclock.h - clock / PLL / flash bring-up.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * SystemClock_PllFlashInit programs the SCG clock generator (0x40000000
 * block: SIRC / FIRC / SOSC / SPLL), the FTFC flash controller
 * (0x40020000 / 0x40034000: unlock/cmd at +0xc0/+0xc8), applies the
 * factory trim read from the flash IFR (~0x3F000 region, present on-chip
 * but outside this image), and publishes SystemCoreClock to RAM
 * (0x20001654 and 0x20000010). All register access is verbatim VanMoof
 * MMIO (volatile pointers at absolute addresses).
 */

/*
 * Bring up the system clock tree and flash wait-states. // 0x00001244
 *
 * Sequence (verbatim from the OEM image):
 *   - FTFC flash housekeeping (0x40020000 +0xc8/+0xc0).
 *   - Enable / trim the fast IRC (FIRC) and slow IRC (SIRC) in the SCG.
 *   - Apply factory IRC trim from flash IFR (0x3FCE8/0x3FD30/0x3FD40).
 *   - Configure the system oscillator (SOSC) range from the measured
 *     crystal frequency.
 *   - Select the flash read clock divider from an OEM lookup table and
 *     program FTFC FOPT / wait-states (0x40034000, 0x40000400).
 *   - Configure and lock the SPLL (0x40000580 block) for 96 MHz and verify
 *     the PLL output is within tolerance; on failure fall back to a busy
 *     wait and skip the PLL switch.
 *   - Set every SCG clock divider (RUN/HSRUN/VLPR config at 0x40000300 /
 *     0x40000380 / 0x400003c0) and publish SystemCoreClock = 96 MHz.
 */
void SystemClock_PllFlashInit(void);

#endif /* USER_ECU_SYSCLOCK_H */
