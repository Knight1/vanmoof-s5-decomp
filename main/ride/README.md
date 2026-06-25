# ride — pedal-assist controller (i.MX8)

`/usr/bin/ride` — the VanMoof S5 i.MX8 pedal-assist controller. Stripped AArch64
C++ ELF (`AARCH64:LE:64:v8A`, base `0x100000`, 465 functions), package
`vmxs5-embedded-ride`, Ghidra `/S5-v1.5/OS/ride`.

Reads pedal torque/cadence + battery state, gates motor assist
(`PoweredOn && Pedalling && SufficientSOC`), and drives the motor controller over
the **SSP serial protocol** (SLIP + CRC-16/MODBUS, `/dev/ttymxc3`); reads sensors
over CANopen OD; publishes `ride/info/*` telemetry.

- **Architecture + SSP protocol + assist gating:** [`../docs/ride.md`](../docs/ride.md)
- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/ride_program.json`](ghidra/exports/ride_program.json)

## Key anchors
- `common_logf` `0x147630`; `ride_main` `0x10a1f0`
- `ride_service_ctor` `0x1106e0`; `ride_service_should_enable_motor` `0x11bb00`
- SSP: `ssp_protocol_ctor` `0x122630`, `ssp_receive_loop` `0x122d50`, `ssp_send_loop` `0x124100`, `ssp_crc16` `0x12a340`
- `ride_service_publish_telemetry` `0x1129f0`; `power_compute_max_discharge_current` `0x118d00`

## Build
`make` — compiles the 8 reconstructed TUs in `src/` clean under
`gcc -std=c11 -Wall -Wextra -Wpedantic -Iinclude` (behaviour-oriented C, like the
other i.MX8 targets). Canonical model: `include/ride_common.h`.

## Scope
The VanMoof app logic (SSP protocol, assist gate/filter, power gates, sensor
parse, strategy lookup) is reconstructed to C. The C++ runtime
(STL/`std::function`/`std::thread`/json), the bike-VM/CANopen-OD framework and the
shared `common`/`lib` are vendor. The assist-curve constants are data-driven and
live in runtime `.bss` (not in the static image).
