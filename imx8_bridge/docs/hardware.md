# imx8_bridge — hardware & firmware map

The **SPI↔CAN bridge co-processor** of the VanMoof S5/A5, an **NXP LPC55S69**
(LPC5500, Cortex-M4F). It is a node on the bike's CAN-FD fleet and a **SPI slave**
to the i.MX8 Linux main module: the Linux `spi-can-if-linux` service (pkg
`vmxs5-embedded-spi-can-bridge`, see `../../main/spi_can_bridge/`) talks SPI to
this MCU, which relays frames to/from the Cortex-M sub-ECUs (motors, lights, lock,
e-shifter, power boards, `user_ecu`).

## Image

`opt/devices_fw/imx8_bridge.20240129.145222.1.5.0.main.v1.5.0-main.bin`
- 24,384 bytes (`0x5f40`), **raw** — no MCUboot/PEGA wrapper; offset 0 is the
  Cortex-M vector table.
- `sha256 584d15ab…` · build `Jan 29 2024 14:50:32` · version `1.5.0.main`.
- One flat `ram` region `0x0–0x5f3f` in Ghidra (`ARM:LE:32:Cortex`, base 0).

## CPU / RTOS (confirmed in Ghidra)

- **ARM Cortex-M** (Thumb-2). Vector table: initial **SP = `0x20008000`** (top of
  32 KiB SRAM), reset → `0x0dc0`. Faults: NMI `0x0d84`, HardFault `0x0d88`,
  MemManage `0x0dac`, BusFault `0x0db0`, UsageFault `0x0db4`.
- **FreeRTOS Cortex-M port** — `vPortSVCHandler` `0x0f60`, `xPortPendSVHandler`
  `0x0f00` (TCB context switch `pxCurrentTCB → [+0x10] → [+0x8]`),
  `xPortSysTickHandler` `0x5d7e`. A `…FromISR` notify lives at `0x0e04`; the
  common ISR-exit epilogue (PendSV yield) is `0x15c0`.
- **61 external IRQ slots** (NVIC table `0x40`–`0x134`): a block of 4-byte
  default-handler thunks (`0x0fc4`–`0x108c`) plus the real peripheral ISRs
  (`0x1464`, `0x1498`–`0x15e8`, `0x1648`–`0x16d8`, `0x1708`–`0x1798`, …). Each
  real ISR reads its driver/event object from a literal global, bumps a counter,
  calls the `…FromISR` notify, and yields via `0x15c0`.

## Application tasks (VanMoof)

The rodata tail (`0x5dc0`–`0x5f3f`) carries the FreeRTOS task-name strings. Three
are VanMoof application tasks; the rest are stock kernel:

| Name | String addr | Body | Role (decoded) |
|---|---|---|---|
| `vm`    | `0x5f03` | `spi_rx_send_loop 0x2938`     | SPI-rx → CAN-tx bridge (host → fleet) |
| `CanTX` | `0x5f06` | `spi_tx_send_loop 0x2c5c`     | CAN-rx → SPI-tx bridge (fleet → host) |
| `can`   | `0x5f0c` | `can_rx_dispatch_loop 0x2eb8` | CAN-FD receive → session dispatch |
| `IDLE`    | `0x5f10` | — | FreeRTOS idle (vendor) |
| `Tmr Svc` | `0x5f15` | — | FreeRTOS timer service (vendor) |
| `TmrQ`    | `0x5efe` | — | FreeRTOS timer-command queue name (vendor) |

All three are spawned inside `vm_can_init 0x3ad8` via the VanMoof task-create
primitive `queue_msg_enqueue 0x214c` (allocates a ~`0x6c` TCB + stack, copies the
name, builds the initial ARM exception frame), then the scheduler is started with
`vPortStartFirstTask 0x2d0`. The name strings are reached via ADR (not absolute
32-bit literals — a byte-search for `06 5f 00 00` is empty).

## Peripheral map (NXP LPC55S69, confirmed from the driver code)

- **SYSCON @ `0x40000000`** — AHBCLKCTRL0 clock-enable at `+0x220`, AHBCLKDIV
  `+0x38c`, PLL0SSCG0 `+0xa18`, DEVICE_ID `+0x2a0` (read to select the
  oscillator/PLL source for the CAN baud-rate prescaler). `vm_can_init 0x3ad8`
  writes enable bits `0x800/0x2000/0x4000/0x8000` to `+0x220`, then calls the
  per-peripheral clock-enable helper `clock_enable_peripheral 0x4d94(periph_id)`.
- **IOCON @ `0x40001000`** — pin-mux. CAN bus pins PIO0_2/3/4/6/9 and the
  SPI/other pins PIO0_33/34/35/53 are set to `func=1, DIGIMODE`.
- **DMA0 @ `0x40004000`** — CTRL `+0x0`, SRAMBASE `+0x20`; channel/trigger masks
  for the CAN-FD DMA channels.
- **FlexComm0–14 @ `0x40086000`–`0x4009F000`** (SPI/USART/I²C) — the SPI-slave to
  the i.MX8 lives here. Instance table @ `0x5e00` (7 × 16-byte `base / base-dup /
  IRQ / clk-idx`):

  | base | IRQ | clk | FlexComm |
  |---|---|---|---|
  | `0x40086000` | `0x0E` | 0 | FLEXCOMM0 |
  | `0x40087000` | `0x0F` | 1 | FLEXCOMM1 |
  | `0x40089000` | `0x11` | 3 | FLEXCOMM3 |
  | `0x4008A000` | `0x12` | 4 | FLEXCOMM4 |
  | `0x40097000` | `0x14` | 6 | FLEXCOMM8 (HS-SPI) |
  | `0x40098000` | `0x15` | 7 | FLEXCOMM9 |
  | `0x4009F000` | `0x3B` | 8 | FLEXCOMM14 |

- **Two Bosch M_CAN CAN-FD controllers** — standard register map: CCCR `+0x18`
  (INIT/CCE/TXP/EFBI), NBTP `+0x1c` (nominal timing), DBTP `+0x0c` (data timing),
  TEST `+0x10`, TDCR `+0x48`, IE `+0x54`, ILS `+0x58`, ILE `+0x5c`, XIDAM `+0xa0`,
  RXBC `+0xbc`, RXF1C `+0xc0`, RXF1A `+0xc8`, plus the NXP message-RAM-base
  extension **MRBA `+0x200`**. Bit timing is computed in FP (`×5.75` scale) from
  the DEVICE_ID-selected reference clock; an `0xAA`/`0x55` write-unlock guards the
  config registers. Message buffers map into SRAM at `0x20000200`/`0x20000400`.
- **Clock-frequency table @ `0x5dc0`** (8 × 8-byte `[index, freq]`):
  `11/22/33/44/55/66/84/96 MHz` indexed `0–7` (plus a `12 MHz` crystal constant
  `0x00B71B00` in the tail); used to derive FlexComm baud rates by clock index.
- **NVIC enable list @ `0x5ea0`** — FlexComm IRQs `0x0B–0x12` + `0x1C`, enabled
  by `peripheral_interrupt_init_sweep 0x7b0`.

### ISR driver globals (`0x1878` literal pool)

| slot | RAM ptr | used by |
|---|---|---|
| 0 | `0x20000744` | `spi_timer_isr 0x1464` |
| 1 | `0x2000106C` | `spi_irq_group0 0x1498` |
| 2 | `0x2000075C` | `can_mb_*` (CAN IRQ) |
| 3 | `0x200009AC` | `isr_1708` (CAN MB loop body) |

## `VMFW` firmware header (`0x134`)

Shared VanMoof Cortex-M image format (magic `VMFW`), common to the in-house
sub-ECUs (`elock`, `eshifter`, `frontlight`, `rearlight`, `power_*`, `user_ecu`).
Decoded verbatim:

```
0x134  "VMFW"                  magic
0x138  u32  0x01056000         version (1.5.0-main, packed)
0x13C  u32  0xa83130b3         image CRC32
0x140  u32  0x00005f40         image length (24384 ✓)
0x144  "Jan 29 2024"           build __DATE__
0x150  "14:50:32"              build __TIME__
0x15C  u32  0x00006b34         RAM region size
0x160  u32  0x20000000         SRAM base
       …                       region/size table (0x0c, 0x00006b34, 0x04000000)
```

The `update` per-page-CRC flasher keys off this header (`../../main/docs/update.md`).
