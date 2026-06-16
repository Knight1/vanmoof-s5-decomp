#ifndef USER_ECU_GPIO_IRQ_H
#define USER_ECU_GPIO_IRQ_H

#include <stdint.h>

/*
 * gpio_irq.h — GPIO interrupt-source registration glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M / Cortex-M4F, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * gpio_irq_register() populates one fixed slot in a small RAM registration
 * table at 0x20000668. The caller (main_SystemInit) zero-clears the whole
 * table (vmem_set(0x20000668, 0, 0x30) == 4 * 12 bytes) and then calls this
 * routine four times, once per GPIO interrupt source.
 */

/*
 * gpio_irq_register — initialise one GPIO-IRQ registration slot. // 0x0000159c
 *
 * Slot layout (12 bytes, base = 0x20000668 + seq * 0x0c):
 *   +0x00  uint8_t   seq      (this slot's own index, written last as a byte)
 *   +0x01  uint8_t   bank     (GPIO/PORT bank selector)
 *   +0x02  uint8_t   pin      (pin number: 0x16, 0x15, 0x05, 0x0f)
 *   +0x03  uint8_t   idx      (per-source sub-index)
 *   +0x04  void *    block    (freshly pvPortMalloc(0x0c)'d work block)
 *   +0x08  ...                (remaining bytes left zero by the memset)
 *
 * The slot is first zero-filled, the three descriptor bytes are stored at
 * +1/+2/+3, the slot index is stored at +0, and a 12-byte heap block is
 * allocated and its pointer stored at +4. All four arguments are effectively
 * bytes; `seq` additionally indexes the table.
 */
void gpio_irq_register(uint8_t bank, uint8_t pin, uint8_t idx, uint8_t seq);

#endif /* USER_ECU_GPIO_IRQ_H */
