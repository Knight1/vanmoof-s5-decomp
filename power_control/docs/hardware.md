# power_control — hardware / image facts

> The VanMoof S5/A5 **power-management sub-ECU**. Raw ARM Cortex-M image, base
> `0x0`. Facts below are from the image header + the classification pass
> (`ghidra/exports/power_control_classification.json`).

## Image

| | |
|---|---|
| File | `power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin` |
| Size | 29248 bytes (`0x7240`) |
| Header | shared VanMoof **VMFW** header @ `0x134` (magic `VMFW`, image size `0x7240`, build `Jan 29 2024 14:50:32`) |
| Layout | raw Cortex-M vector table @ `0x0`; `.text` ~`0x564`..`0x6f00`; rodata/data after |
| Image base | `0x0` (no rebase, unlike `ble`) |

## MCU

- **Core:** ARM **Cortex-M4F** (ARMv7-M; Thumb-2 `ldrd`/`bfi`/`movw`, BASEPRI
  masking at `0x20` = `configMAX_SYSCALL_INTERRUPT_PRIORITY`, DSB/ISB). Hard-float
  ABI (project convention; no VFP op observed in the VanMoof slice, which is
  integer-only). Build flags: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`.
- **SRAM:** 32 KB at `0x2000_0000` (initial SP `0x2000_8000`; globals seen at
  `0x2000_06xx`/`0x2000_09xx`).
- **Silicon vendor / part:** **NXP LPC546xx** (Cortex-M4F) — confirmed against
  the bus captures (`vanmoof/canbus` `CANBUS.md`: "Platform MCU: NXP LPC546xx").
  Firmware evidence: an **AIPS** bridge control/ID register at `0x4000_0FFC` and
  an **NXP eDMA** controller (instance 0 @ `0x4008_2000`, instance 1 @
  `0x400A_7000`, +`0x25000` spacing; per-channel TCD stride `0x10` at base+`0x400`;
  channel-enable/interrupt bitmaps at base+`0x30`).
- **CAN:** Bosch **M_CAN** (CAN-FD capable, used as classic CAN; the LPC546xx CAN
  IP) — **confirmed**. Message RAM at driver-base+`0x200`, TXBAR at +`0xd0`, 29-bit
  extended IDs (`id & 0x1fffffff | XTD`); peripheral base **`0x4009_D000`** (per
  the bus doc), reached in the firmware via a driver-struct field (+`0x160`). Bus
  runs at **1 Mbps**. *(An early classification read it as ST bxCAN; that was a
  misread — the `can_tx_task` disassembly and the LPC546xx identity both confirm
  M_CAN.)*
- **System:** Cortex-M SCB at `0xE000_ED04` (ICSR, `PENDSVSET` for FreeRTOS
  `portYIELD`).

## Software stack

- **FreeRTOS** (ARM_CM4F port): `heap_4` (`pvPortMalloc` @ `0x2030`, ~21.5 KB
  `ucHeap`), queues/semaphores (`xQueueGenericSend` @ `0x15dc`,
  `xQueueReceive` @ `0x1a28`), software timers (command dispatch @ `0x2d40`,
  `xTimerCreate`+start @ `0x3658`), task notify (`0x1844`), list.c primitives,
  stream/message buffers, `vPortEnterCritical`/`vPortExitCritical`
  (`0xdb8`/`0xdd4`), `configASSERT` trap (`0x5e68`). Task **`power_control_task`**
  (name @ `0x717d`).
- **NXP SDK/HAL:** eDMA driver, M_CAN driver, a callback-table dispatch layer,
  a flash/EEPROM-emulation page driver (512-byte pages, `0xff` erase fill).
- **libgcc / `__aeabi_*`:** a small cluster of soft helpers.

## VanMoof application layer

15 functions (see `docs/progress.md`): the **power-mode state machine**
(`power_set_mode`) + rail-enable sequencing + capability/status/event frame
builders, an **NTC ADC→temperature** lookup, and the **CAN** request/send
wrappers, TX task, event-record encoder, and a flash/storage page writer.
Everything else is vendor (FreeRTOS / NXP HAL / libgcc), deferred.
