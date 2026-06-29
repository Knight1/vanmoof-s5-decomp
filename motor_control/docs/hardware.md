# motor_control — hardware (TI TMS320F280049C)

The motor-drive controller DSP. Part identified from the standalone SCI flasher
(`../../main/motor_update_lib/` targets a **TMS320F280049C**) and the TI C28x
boot-table image format.

## MCU — TI **TMS320F280049C** (C2000 Piccolo, C28x core)

- **C28x** 32-bit fixed/floating-point DSP core (TMU + FPU), 100 MHz, with the
  **CLA** co-processor — the standard TI part for FOC motor control.
- **Word-addressed**: program/data addresses count 16-bit words, not bytes (a
  25,890-byte file = 12,945 words). This must be modelled when importing.
- On-chip memory (F28004x): M0 RAM `0x0000–0x07FF`, M1 RAM `0x0400–0x07FF`,
  LSx RAM `0x8000–0xBFFF`, GSx RAM `0xC000–0x1BFFF`, **Flash `0x080000–0x0BFFFF`**.
- Motor-control peripherals: ePWM (commutation), eCAP/eQEP (position),
  high-speed ADC + comparators (phase current), and a **CAN/MCAN** module for the
  fleet bus and **SCI** for the bootloader.

## Image format — TI C28x serial boot-table

Header word **`0x08AA`** = the C28x "16-bit data stream" boot key (SCI/flash
boot). Layout: key, 8 reserved words, 2-word entry point, then
`{size, dest_MSW, dest_LSW, data[size]}` blocks, zero-size terminated. The four
blocks all target flash (entry `0x080000`); see `../README.md` for the parsed map.
This is **not** the `VMFW` Cortex-M format the ARM sub-ECUs share.

## Disassembly status

Blocked: this Ghidra has no TMS320C28x SLEIGH (import → "Language not found").
Needs a community C28x processor module or TI CCS to proceed; the README records
the entry point and flash segment addresses to seed that import.
