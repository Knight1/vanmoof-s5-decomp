# user_ecu — VanMoof S5/A5 main vehicle controller

`user_ecu.20240129.145222.1.5.0.main` — the S5's central application MCU (the
generation's equivalent of the S3 `mainware`/"muco"). It runs an RTOS and
orchestrates the bike's subsystems: LED rings, sensors, a digital microphone,
and the control/comms paths to the other ECUs.

## At a glance

| Property | Value |
| --- | --- |
| Core | **ARM Cortex-M4F** (ARMv7-M, VFPv4 single-precision, hard-float) — confirmed via `vfma.f32` |
| MCU family | NXP **S32K-like** (SCG/FTFC/PCC/IFR-trim, 96 MHz from 12 MHz via SPLL); exact SKU open (S32K144 refuted) |
| RTOS | **FreeRTOS** (Cortex-M4F port) — BASEPRI critical sections, `vPortStartFirstTask` (svc #2) |
| Image format | Raw binary, linked at base `0x00000000` |
| Image size | `0x0001a88c` = 108 684 bytes (~106 KB) |
| SRAM | `0x20000000`, initial SP `0x20010000` (≥ 64 KB) |
| Functions (Ghidra) | 160 total; **44 named** (boot/RTOS/comms/control/HAL) |
| Strings | logging-stripped — only the 4 task-name strings survive in-image |

## Vector table (`0x00000000`)

The table at `0x0` is a **decoy** (its Reset slot `0x00000dd4` points into LED
easing-math). The **real** reset handler is `Reset_Handler` @`0x000001d4`, which
relocates the runtime vector table via `VTOR = 0x00000c00`, enables the FPU,
runs `.data`/`.bss`, and `bl main_SystemInit`. (How the CPU first reaches `0x1d4`
and the live `0xc00` table — likely a separate first-stage bootloader not in this
dump — remain open; see `docs/progress.md`.)

## Architecture (workflow-verified)

- **`Reset_Handler`** (`0x1d4`) → **`main_SystemInit`** (`0x44c0`): clocks, GPIO
  mux, peripheral bringup, 8× `xTaskCreate`, 1 ms SysTick, then
  **`vPortStartFirstTask`** (`0x370`, FreeRTOS).
- **Control** — `controlTask_CmdHandler` (`0x1ee0`) runs the **Q16.16** easing
  kernel `ledEasing_ControlUpdate` (`0xc64`) on the L/R LED-ring state structs.
- **Q16.16 math lib** (`1.0 == 0x00010000`) — `q16_mul`, `q16_div`, `q16_sqrt`,
  `q16_exp`, `q16_sigmoid` (`0x835e/0x83b8/0x841e/0xb04/0x847c`).
- **Comms** — I²C master via an IOM engine `iom_i2c_transfer` (`0x7288`); BE
  16-bit words each + CRC-8 (poly `0x31`). See `docs/protocol.md`.
- **FreeRTOS primitives** — `vPortRaiseBASEPRI`, `vPortEnter/ExitCritical`,
  `vTaskSwitchContext_SelectNext`, `xTaskCreate`.

See `docs/architecture.md` for the full overview.

See `docs/` for the full breakdown:
- **`progress.md`** — per-function tracker (source of truth for what's left).
- **`hardware.md`** — memory map, vector table, peripheral/SRAM globals.
- **`protocol.md`** — inter-ECU comms (CAN/UART to `imx8_bridge` & friends).

## Building

Nothing builds yet — this is the analysis scaffold. Once the startup path and
MCU are confirmed, `src/` + `linker_user_ecu.ld` get filled in and:

```bash
make            # build build/user_ecu.bin
make compare OEM_IMAGE=user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
```
