# `motor_sensor` — motor position / current / temperature sensor ECU

The sensor board that reports the **motor's rotor position, phase current and
temperature** to the VanMoof S5/A5 fleet over **CAN-FD**. A raw NXP Cortex-M4
(LPC546xx-class) FreeRTOS firmware in the shared in-house `VMFW` image format.

> Status: **decoded.** Imported into the `vanmoof` Ghidra project
> (`/S5-v1.5/…motor_sensor…bin`, `ARM:LE:32:Cortex`, base 0). Auto-analysis found
> **0 functions** (raw image, no entry map); bootstrapped from the vector table
> and carved by a 9-agent workflow to **218 functions — 92 VanMoof app, 118
> vendor, 8 unsure** (`ghidra/exports/motor_sensor_classification.json`). The
> architecture (5 tasks + the sensor/CAN/flash logic) is mapped below. C
> reconstruction of the app is the next pass (as done for `imx8_bridge`).

## Image

`opt/devices_fw/motor_sensor.20240129.145222.1.5.0.main.v1.5.0-main.bin`
- 25,092 bytes (`0x6204`), raw — offset 0 is the Cortex-M vector table.
- build `Jan 29 2024 14:50:32` · version `1.5.0.main` · VMFW CRC `0xede5da94`.

## CPU / RTOS (confirmed)

- **ARM Cortex-M4** (Thumb-2), base 0, initial **SP `0x20008000`**, reset
  `0x0dc0`. Faults NMI `0x0d84` / HardFault `0x0d88` / MemManage `0x0dac` /
  BusFault `0x0db0` / UsageFault `0x0db4`.
- **FreeRTOS Cortex-M port** — `vPortSVCHandler` `0x0f60`, `xPortPendSVHandler`
  `0x0f00`, `xPortSysTickHandler` `0x5b80` (same port + reset address as the
  `imx8_bridge` sibling — shared startup/RTOS scaffolding). ~60 external IRQ
  vectors `0x40`–`0x133`.
- `xTaskCreate` = `FUN_000029ec`; `pvPortMalloc` = `FUN_000027f4`;
  `memset` = `FUN_00005f62`.

## Application tasks (VanMoof)

Task-name strings in the rodata tail (`0x615e`+):

| Name | String | Role |
|---|---|---|
| `motor_sensor_task` | `0x6176` | main app task — sample the sensors, build & report CAN frames |
| `CanTX` | `0x616c` | CAN transmit task (drains the TX queue → controller) |
| `can`   | `0x6163`… | CAN receive / dispatch task |
| `VMKYS` | `0x6163` | VanMoof key/aux sensor sub-task (ties to the BLE-GATT link) |
| `vm`    | `0x6169` | VanMoof supervisor task |
| `IDLE` / `Tmr Svc` / `TmrQ` | — | FreeRTOS (vendor) |

`main` = **`FUN_0000375c`** (`0x375c`–`0x4cf9`, ~4.5 KB): peripheral bring-up
(clocks, M_CAN, ADC, SCT/timer, GPIO), creates the tasks via `xTaskCreate`, then
starts the scheduler.

## Peripheral map (tentative — LPC546xx-class)

| Base | Peripheral | Notes |
|---|---|---|
| `0x40006000` | **M_CAN / FDCAN** | the CAN-FD reporting bus; bit-timing `+0x88/+0x8c`, filters, frame I/O |
| `0x400A0000` | ADC / FLEXCOMM | current & temperature sampling (conversion/status/sample regs) |
| `0x40008000` | SCT / CTimer | Hall/encoder capture for rotor position; also SPI-flash path |
| `0xf400`     | **magnetic angle/position sensor** | 12-byte position register block (`sensor_position_read`) |
| `0x40004000` | GPIO controller | 8-channel GPIO IRQ bank (ISRs `0x574`–`0x6c4`) |
| `0x40000000` | SYSCON / clock | AHBCLKDIV/clock-enable, IOCON pin-mux, GPIO IN/SET/CLR |
| WWDT | watchdog | `0xAA`/`0x55` feed sequence |

## Subsystems (decoded)

- **Sensor sampling** — magnetic angle/position sensor read (`@0xf400`), ADC
  current/temperature, against a **422-byte calibration/lookup table** at
  `0x5f9a`–`0x613f`.
- **CAN-FD reporting** — `can_controller_init` (`0x15fc`), TX manager loop
  (`0x1d5c`, the `CanTX` task), frame submit/encode (`0x59fa`/`0x5a70`),
  health/status checks (`can_get_status_flags 0x4b8`, `can_is_fd_mode 0x754`).
- **Flash update path** — `flash_write_sector` (0x200-byte sectors, ≤0x7400),
  page verify/advance — the `update` service's per-page-CRC relay.
- **Watchdog** — `watchdog_feed_timer_cb` (`0xAA`/`0x55` to WWDT+8).
- **BLE-GATT / key-sensor link** (flagged `unsure`) — `gatt_char_0x87/0x91
  read_and_notify` + the `VMKYS` task; a software interface to the nRF52
  co-processor, or a shared comms lib.

## `VMFW` header (`0x134`)

Shared VanMoof Cortex-M image format (magic `VMFW`, version `0x01056000` =
1.5.0-main, length `0x6204`, `Jan 29 2024 14:50:32`). See `docs/hardware.md`.

## Layout

```
motor_sensor/
  README.md                 — this file
  docs/hardware.md          — MCU + peripheral + VMFW map
  docs/progress.md          — per-function status
  ghidra/exports/motor_sensor_classification.json — 218-function app/vendor split
```

## Open / next

- [ ] Reconstruct the VanMoof app to C (sensor sampling + CAN reporting + tasks),
      like the `imx8_bridge` pass.
- [ ] Confirm the exact MCU part + peripheral bases (the `0x40006000` CAN /
      `0x400A0000` ADC bases need pinning against the LPC546xx memory map).
- [ ] Resolve the BLE-GATT/`VMKYS` link — on-board nRF link vs shared comms lib.
- [ ] Cross-ref the CAN report frames against `vanmoof/canbus` `CANBUS.md`.
