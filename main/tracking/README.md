# tracking — anti-theft / location (i.MX8)

`/usr/bin/tracking` — the VanMoof S5 i.MX8 anti-theft / tracking service.
Stripped AArch64 C++ ELF (`AARCH64:LE:64:v8A`, base `0x100000`, 424 functions),
package `vmxs5-embedded-tracking`, Ghidra `/S5-v1.5/OS/tracking`.

A small theft state machine (OFF / AUTO / THEFT) fed by IMU movement, inbound
SMS, and cellular-location polling; reports over MQTT to `ux` and the modem.

- **Architecture + MQTT map:** [`../docs/tracking.md`](../docs/tracking.md)
- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/tracking_program.json`](ghidra/exports/tracking_program.json)

## Key anchors
- `common_logf` `0x135b10`
- `tracking_main` `0x107c60`
- `tracking_service_app_ctor` `0x112e10` (derives `common::StateClient`); state enum 0=OFF/1=AUTO/2=THEFT
- `tracking_service_set_state` `0x113810`; `tracking_state_to_string` `0x123890`
- `Movement` `0x111df0` (IMU), `CellLocator` `0x10af70` (cellular)

## Build
`make` — compiles the 5 reconstructed TUs in `src/` clean under
`gcc -std=c11 -Wall -Wextra -Wpedantic -Iinclude` (behaviour-oriented C, like
`main/update`/`main/power`). The framework model is `include/tracking_common.h`.

## Scope
The VanMoof app logic (the 3-state theft machine, cell-locator, SMS parse,
movement) is reconstructed to C. nlohmann::json / libstdc++ / `std::function`
glue and the shared `common`/`lib` framework are modelled opaque (vendor).
