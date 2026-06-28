# `imx8_bridge` — i.MX8 ↔ CAN-FD bridge MCU (NXP LPC55S69)

The bridge that lets the Linux main module reach the bike's **CAN-FD** fleet.
Linux talks **SPI** to this MCU (`spi-can-if-linux` / `spi-can-bridge@bridge.service`,
see `../main/spi_can_bridge/`), and this MCU is a node on the bike's **CAN-FD**
bus to the Cortex-M sub-ECUs (motors, lights, lock, e-shifter, power boards,
`user_ecu`).

> Status: **decoded.** Raw image imported into the `vanmoof` Ghidra project
> (`/S5-v1.5/…imx8_bridge…bin`, `ARM:LE:32:Cortex`, base 0). Ghidra
> auto-analysis *fails* on the bare image (no entry map); it was bootstrapped by
> hand from the vector table and then **carved + classified by a 9-agent map
> pass: 146 functions — 60 VanMoof app, 86 vendor** (`ghidra/exports/
> imx8_bridge_classification.json`). Host MCU, FreeRTOS port, the dual CAN-FD
> controllers, the SPI-slave framing and the 3 application tasks are all
> Ghidra-confirmed (below). The VanMoof bridge app is reconstructed to C in
> `src/` (vendor SDK + FreeRTOS stay off-tree).

## Image

`opt/devices_fw/imx8_bridge.20240129.145222.1.5.0.main.v1.5.0-main.bin`
- 24,384 bytes (`0x5f40`), raw — **no MCUboot/PEGA wrapper**; offset 0 is the
  Cortex-M vector table.
- `sha256 584d15ab…` · build `Jan 29 2024 14:50:32` · version `1.5.0.main`.

## Host MCU — NXP **LPC55S69** (confirmed)

Pinned down from the peripheral map (not a guess): **SYSCON @ `0x40000000`**
(AHBCLKCTRL0 `+0x220`, AHBCLKDIV `+0x38c`, PLL0SSCG0 `+0xa18`, DEVICE_ID `+0x2a0`),
**IOCON @ `0x40001000`**, **DMA0 @ `0x40004000`**, **FlexComm0–14 @
`0x40086000`–`0x4009F000`** (SPI/USART/I²C), and **two Bosch M_CAN CAN-FD
controllers** with the standard register map (CCCR `+0x18`, NBTP `+0x1c`, DBTP
`+0x0c`, IE `+0x54`, plus the NXP message-RAM-base extension `MRBA +0x200`). This
is the device-tree `lpc55sxx@0` SPI satellite. The rodata tail carries a
clock-frequency table (`11/22/33/44/55/66/84/96 MHz`, indexed 0–7) and a
FlexComm instance table (base / IRQ / clock-index). Full peripheral map:
`docs/hardware.md`.

## Application architecture (decoded)

The VanMoof app is a thin layer over the LPC55 SDK + FreeRTOS: **`main` =
`vm_can_init` (`0x3ad8`)** brings up the clocks/IOCON/DMA/dual-MCAN/SPI-FlexComm,
spawns three tasks, then starts the scheduler (`vPortStartFirstTask 0x2d0`).

| Task | Entry | Body | Role |
|---|---|---|---|
| **`vm`**    | `0x2938` | `spi_rx_send_loop`    | drains SPI frames from the i.MX8 host → transmits them on CAN-FD |
| **`CanTX`** | `0x2c5c` | `spi_tx_send_loop`    | drains the CAN-RX queue → formats & sends SPI frames back to the host |
| **`can`**   | `0x2eb8` | `can_rx_dispatch_loop`| receives CAN-FD frames → dispatches to the matching session/handler |

- **CAN-TP session layer** — `can_session_open/write/read/rx_complete/
  tx_complete/clear_filters` over a **`0x14`-stride session table** with a
  round-robin scheduler (`can_session_table_advance 0x3a6c`) and a periodic
  FreeRTOS timer. `can_rx_frame_handler 0x37e4` decodes the 3-byte CAN-ID and
  runs the per-session state machine (no-session / connecting / connected,
  buffering into `0x4c`-stride slots).
- **SPI-slave framing to the host** — `spi_tx_frame_send 0x55f0` builds a
  13-byte SPI frame (session-id, flags, mode byte); `spi_frame_write_chunked
  0x526a` splits payloads into ≤8-byte chunks; ring buffers
  (`ring_buf_read/write/…`) and per-channel objects (`spi_channel_alloc/create`)
  carry the traffic. `spi_tx_enqueue 0x584a` is the thread/ISR-safe enqueue.
- **CAN-FD transmit** — `can_fd_transmit 0x23a4` (classical + FD, BRS via
  `MCAN_CalculateBitTimingParam`), `can_tx_slot_alloc 0x2338` (≤3 pending TX
  slots), `can_tx_send_msg 0x377c` (diagnostic event frames).
- **Flash-update path** — `flash_session_open/write_sector/verify_and_commit`:
  the bridge can be reflashed *and* relays firmware to the sub-ECUs (the
  `update` service's per-page-CRC flow, see `../main/docs/update.md`).

## ISR → task data path

Each real peripheral ISR reads its driver/event object from a literal global,
posts to a FreeRTOS queue / notifies a task (`…FromISR` notify `0x0e04`), then
yields via the common epilogue `0x15c0` (writes `PENDSVSET` to `SCB->ICSR
0xe000ed04`). The CAN message-buffer group ISR (`can_mb_group_isr 0x15b8`) is
entered at several offsets by the IRQ20/21/32-35/42-43 vectors (multi-entry
shared handler); `spi_timer_isr 0x1464` and the FlexComm SPI ISRs feed the SPI
side. So: **peripheral ISR → FreeRTOS queue/notify → vm/CanTX/can task**.

## `VMFW` firmware header (`0x134`)

Shared VanMoof Cortex-M image format (magic `VMFW`), common to the in-house
sub-ECUs (`elock`, `eshifter`, `frontlight`, `rearlight`, `power_*`, `user_ecu`).
Decoded verbatim — see `docs/hardware.md`. The `update` per-page-CRC flasher keys
off it.

## Layout

```
imx8_bridge/
  README.md                 — this file
  Makefile                  — compile-gate build (app TUs only; vendor off-tree)
  include/                  — imx8_bridge.h, compiler.h
  src/                      — VanMoof bridge app reconstructed to C
  docs/hardware.md          — MCU + peripheral + VMFW map
  docs/progress.md          — per-function status
  ghidra/exports/imx8_bridge_classification.json — 146-function app/vendor split
```

## Open / next

- [ ] Finish the C reconstruction of the SPI-slave wire format + CAN-TP session
      framing (struct layouts from the deep-decode pass).
- [ ] Cross-ref the SPI frame format against the Linux side
      (`../main/spi_can_bridge/` `spim_channel.cpp` + `lib/src/tp`).
- [ ] Confirm the `vm_can_init` task-create primitive (`queue_msg_enqueue
      0x214c`) vs stock `xTaskCreate`.
