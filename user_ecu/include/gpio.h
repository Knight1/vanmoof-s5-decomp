#ifndef USER_ECU_GPIO_H
#define USER_ECU_GPIO_H

#include <stdint.h>

/*
 * GPIO pin configuration + GPIO interrupt-dispatch glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS). Direct MMIO is VanMoof bare-metal
 * glue reproduced verbatim:
 *   - GPIO output regs base 0x4008E000 (DATA/DATASET/DATACLEAR style)
 *   - PORT interrupt-status / config regs reached through per-pin base ptrs
 *     stored in the runtime handler table.
 */

/*
 * Configure a single GPIO output pin.
 *
 *   pin   : bit position (0..31) within the GPIO output data register.
 *   flags : 2-byte selector read by the OEM:
 *             flags[0] == 0  -> clear the pin's bit in the data register
 *             flags[0] != 0  -> set the pin's bit; flags[1] selects which
 *                               bit-band register is written first:
 *                                 flags[1] == 0 -> CLEAR-aux reg (+0x280)
 *                                 flags[1] != 0 -> SET-aux   reg (+0x200)
 *                               then OR the bit into the data register.
 *
 * Enables the peripheral clock-gate (PCC index 0xe) before touching MMIO.
 * // 0x000065a0
 */
void gpio_pin_config(uint32_t pin, const char *flags);

/*
 * Walk the runtime GPIO IRQ handler table for the active GPIO bank, and for
 * every registered entry whose PORT interrupt-status bit is set: clear that
 * status bit (write-1-to-clear) and invoke the registered callback.
 *
 * Intended to be called from a GPIO bank ISR (the trampolines below).
 * // 0x00002018
 */
void gpio_irq_dispatch(void);

/*
 * GPIO bank IRQ trampolines (one per GPIO/PORT bank). Each maps its bank
 * base to a 0..8 index via irqn_to_gpio_index() and tail-calls the per-bank
 * handler from the runtime tables. // 0x00002100 .. 0x00002280
 *
 * NOTE: gpio_bank_irq_trampoline_9 (0x000022b0) is NOT a trampoline; it is a
 * distinct pin-interrupt-configuration routine. It is named per the module
 * spec but takes real arguments and returns a status code. See gpio.c.
 */
void gpio_bank_irq_trampoline_0(void);
void gpio_bank_irq_trampoline_1(void);
void gpio_bank_irq_trampoline_2(void);
void gpio_bank_irq_trampoline_3(void);
void gpio_bank_irq_trampoline_4(void);
void gpio_bank_irq_trampoline_5(void);
void gpio_bank_irq_trampoline_6(void);
void gpio_bank_irq_trampoline_7(void);
void gpio_bank_irq_trampoline_8(void);
int  gpio_bank_irq_trampoline_9(volatile uint32_t *port_base, uint32_t pin_sel);

#endif /* USER_ECU_GPIO_H */
