# ux — UX orchestrator (i.MX8)

`/usr/bin/ux` — the VanMoof S5 i.MX8 user-experience service. Stripped AArch64
C++ ELF (`AARCH64:LE:64:v8A`, base `0x100000`, 602 functions), package
`vmxs5-embedded-ux`, imported in Ghidra at `/S5-v1.5/OS/ux`.

It is the bike's behavioural brain: a polymorphic **6-state strategy machine**
(`UXService`) that coordinates lock, lights, sound, sensors, ride telemetry,
alarm/theft, Apple Find-My, the power button and the power state machine — all
over the MQTT message bus.

- **Architecture + MQTT map + state machine:** [`../docs/ux.md`](../docs/ux.md)
- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/ux_program.json`](ghidra/exports/ux_program.json)

## Key anchors
- `common_logf` `0x1a9590` — `(file, line, level{1=DBG/2=INFO/3=WARN/4=ERR}, fmt, …)`
- `main` `0x10ede0` (carved from the `_INIT_1` static-init blob)
- `ux_service_ctor` `0x12e3e0` — `UXService` (vtable `0x1fae00`, typeinfo `0x1fad88`)
- State enum @ `UXService+0x1090`; active `IStateStrategy*` @ `+0x1088`
- `To<State>` virtuals `0x12dbb0`–`0x12e030`; strategy ctors per `state_*_strategy.cpp`

## Build
`make` — compiles the 21 reconstructed TUs in `src/` clean under
`gcc -std=c11 -Wall -Wextra -Wpedantic -Iinclude` (behaviour-oriented C, like
`main/update`/`main/power`/`main/tracking`). The canonical framework model is
`include/ux_common.h`; each module has its own `include/<module>.h`.

## Scope
The VanMoof app logic (the `UXService` state machine, the 7 strategies, and the
subsystem clients) is reconstructed to C. The C++ runtime (STL, `std::function`
glue, nlohmann::json), the bike-VM/CAN bus, the sound/LED/light drivers and the
shared `common`/`lib` framework are modelled opaque (vendor).
