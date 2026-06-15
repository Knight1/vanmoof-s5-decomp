#ifndef USER_ECU_PCC_H
#define USER_ECU_PCC_H

#include <stdint.h>

/*
 * pcc.h — peripheral bring-up glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Thin bare-metal helpers over the peripheral clock-gate controller
 * (0x40000000 block), a PORT clock/ready handshake, the NVIC ISER bank
 * (0xE000E100) and a GPIO bank-base -> index map. All register access is
 * verbatim VanMoof MMIO (volatile pointers at absolute addresses).
 */

/*
 * pcc_gate_enable — enable a peripheral clock gate from a packed index.
 * // 0x00008a64
 *
 * The argument packs a word offset and a bit position:
 *   word_index = (idx >> 8) + 0x88     (register = 0x40000000 + word_index*4)
 *   bit        = idx & 0xff
 * Writes (1u << bit) to that clock-gate register.
 */
void pcc_gate_enable(uint32_t packed_index);

/*
 * pcc_gate_set — conditionally latch a clock-gate bit. // 0x00008aca
 *
 * ABI: (block_base, gate_index). mask = 1u << (gate_index & 0xff).
 * If the bit is clear in block_base[0] AND already set in block_base[9]
 * (offset 0x24), writes mask back to block_base[9]. Used to (re)assert a
 * gate-enable bit only when the controller reports it valid but not yet live.
 */
void pcc_gate_set(volatile uint32_t *block_base, uint32_t gate_index);

/*
 * port_clock_wait — PORT clock-gate enable + ready handshake. // 0x000084cc
 *
 * The argument packs a word offset and a bit position:
 *   off = (arg >> 16) * 4   (added to the absolute base addresses below)
 *   bit = arg & 0xffff      (only the low 5 bits matter for the shift)
 * mask = 1u << bit. Writes mask to (0x40000120 + off) and spin-waits until the
 * bit reads back set in the status register (0x40000100 + off); then writes
 * mask to (0x40000140 + off) and spin-waits until the bit reads back clear.
 */
void port_clock_wait(uint32_t packed_index);

/*
 * nvic_irq_enable — set the NVIC ISER bit for an IRQ number. // 0x000078d4
 *
 * For a non-negative IRQ number, writes (1u << (irq & 0x1f)) to
 * ISER[irq >> 5] at base 0xE000E100. Negative IRQ numbers are ignored
 * (the OEM table marks "no interrupt" with a negative entry).
 */
void nvic_irq_enable(uint32_t irq);

/*
 * gpio_base_to_bank — map a GPIO bank base address to a 0/1/2 index.
 * // 0x00001ffc
 *
 * 0x40082000           -> 0
 * 0x40082000 + 0x25000 -> 1   (0x400A7000)
 * anything else        -> 2
 */
uint32_t gpio_base_to_bank(uint32_t bank_base);

#endif /* USER_ECU_PCC_H */
