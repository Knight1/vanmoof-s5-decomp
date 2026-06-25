# monitor — component health/version supervisor (i.MX8)

`/usr/bin/monitor` — the VanMoof S5 i.MX8 component supervisor. Stripped AArch64
C++ ELF (`AARCH64:LE:64:v8A`, base `0x100000`, ~748 functions), package
`vmxs5-embedded-monitor`, Ghidra `/S5-v1.5/OS/monitor`.

Watches the sub-ECU/components (BLE, LPC Cortex-M devices, modem): heartbeat
aliveness, firmware/bootloader/vendor versions, GPIO reset lines, and a status
table published over MQTT.

- **Architecture + component model + GPIO reset bank:** [`../docs/monitor.md`](../docs/monitor.md)
- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/monitor_program.json`](ghidra/exports/monitor_program.json)

## Key anchors
- `main` `0x109a98`; `monitor_service_ctor` `0x120ac0`; `monitor_service_run` `0x120fe0`
- `monitor_service_supervise_components` `0x1204c0`; `monitor_service_on_heartbeat` `0x1208f0`
- GPIO reset: `gpio_export_and_configure` `0x138bd0`, `gpio_set_value` `0x139180`, `component_reset_lines_ctor` `0x120310` (pins 1,0,10,11)
- `monitor_print_component_status_table` `0x1221f0`

## Scope
High-level reconstruction (function naming + architecture). The per-component
bodies (ble/lpc_device/modem) are inlined under `-O2` and entangled with vendor
mosquitto + nlohmann-json glue — documented by behaviour + fingerprints, not 1:1
functions. The C++ runtime / mosquitto / json and the shared `common`/`lib` are
vendor.
