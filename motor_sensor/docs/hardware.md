# motor_sensor — hardware & firmware map

The **motor position / current / temperature sensor ECU** of the VanMoof S5/A5.
A raw NXP **Cortex-M4 (LPC546xx-class)** FreeRTOS firmware that samples the motor's
rotor-angle sensor + phase-current/temperature ADC and reports them on the bike's
**CAN-FD** fleet bus. Shares the in-house `VMFW` Cortex-M image format and the same
FreeRTOS port + startup as the other sub-ECUs (`elock`, `imx8_bridge`, …).

## Image

`opt/devices_fw/motor_sensor.20240129.145222.1.5.0.main.v1.5.0-main.bin`
- 25,092 bytes (`0x6204`), raw — offset 0 is the Cortex-M vector table.
- `Jan 29 2024 14:50:32` · version `1.5.0.main`. Imported to Ghidra
  (`ARM:LE:32:Cortex`, base 0); auto-analysis found 0 functions → bootstrapped.

## CPU / RTOS (confirmed in Ghidra)

- **ARM Cortex-M4** (Thumb-2). Vector table: SP `0x20008000` (@0x0), Reset `0x0dc0`
  (@0x4). Faults: NMI `0x0d84`, HardFault `0x0d88`, MemManage `0x0dac`, BusFault
  `0x0db0`, UsageFault `0x0db4`.
- **FreeRTOS Cortex-M port**: `vPortSVCHandler` `0x0f60`, `xPortPendSVHandler`
  `0x0f00`, `xPortSysTickHandler` `0x5b80`. ~60 external IRQ vectors `0x40`–`0x133`
  (peripheral ISRs `0x0fc4`–`0x10b0`, `0x1144`–`0x12f4`; one at `0x1b18`).
- `xTaskCreate` `0x29ec`, `pvPortMalloc` `0x27f4`, `memset` `0x5f62`,
  `freertos_pendsv_trigger` `0x4ee2` (writes `0x10000000` to SCB ICSR `0xe000ed04`).

## Application tasks

Rodata task-name strings (`0x615e`+): `TmrQ`@`0x615e`, `VMKYS`@`0x6163`,
`vm`@`0x6169`, `CanTX`@`0x616c`, `can`@`0x6172`, **`motor_sensor_task`@`0x6176`**,
`IDLE`@`0x6188`, `Tmr Svc`@`0x618d`. Five are VanMoof app tasks; the rest are
stock FreeRTOS. All created from `motor_sensor_hw_init_and_schedule` (`0x375c`)
via `xTaskCreate`.

## Peripheral map (tentative)

| Base | Peripheral | Registers / use |
|---|---|---|
| `0x40006000` | **M_CAN / FDCAN** | NBTP/DBTP `+0x88/+0x8c`, mode `+0xd4/+0xe0`, filters, frame I/O — the report bus |
| `0x400A0000` | ADC / FLEXCOMM | conversion status + sample regs `+0x10..+0x24`, PLL `+0xe0..+0xfc` |
| `0x40008000` | SCT / CTimer | rotor-position (Hall/encoder) capture; SPI-flash path |
| `0xf400`     | magnetic angle/position sensor | 12-byte position register block (`sensor_position_read`) |
| `0x40004000` | GPIO controller (port 0) | 8-channel GPIO IRQ bank |
| `0x40000000` | SYSCON / clock / IOCON | AHBCLKDIV, clock-enable `+0x220`, IOCON pin-mux, GPIO IN/SET/CLR `+0x100/+0x120` |
| WWDT | watchdog | `0xAA`/`0x55` feed |
| `0xe000e000` | NVIC / SysTick / SCB | ISER `0xe000e100`, SysTick `0xe000e010`, SHPR3 `0xe000ed20`, ICSR `0xe000ed04` |

> Exact MCU part is tentative: the FreeRTOS port + `VMFW` + build match the
> LPC546xx sub-ECU family (`elock` is confirmed LPC546xx), but the `0x40006000`
> CAN / `0x400A0000` ADC bases don't cleanly match a stock LPC546xx map — to be
> pinned down against the silicon memory map.

## Calibration data

A **422-byte calibration / ADC lookup table** at `0x5f9a`–`0x613f` (structured
arithmetic sequences, not code) precedes the string table — used by the
sensor-sampling/scaling path.

## `VMFW` header (`0x134`)

Shared VanMoof Cortex-M image format: magic `VMFW`, version `0x01056000`
(1.5.0-main), CRC `0xede5da94`, length `0x00006204` (25092 ✓), build
`Jan 29 2024` / `14:50:32`, SRAM base `0x20000000`. The `update` per-page-CRC
flasher keys off it.
