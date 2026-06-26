# monitor service (component health/version supervisor)

`/usr/bin/monitor` is the i.MX8's **component supervisor** — it watches every
sub-ECU/component (BLE chip, the LPC Cortex-M devices, the modem), tracks their
aliveness via heartbeats, reads their firmware/bootloader/vendor versions, drives
their **GPIO reset lines**, and publishes a component status table + versions over
MQTT. Stripped AArch64 C++ ELF (base `0x100000`, ~748 functions after gap
recovery, Ghidra `/S5-v1.5/OS/monitor`), built from `devices/main/monitor/src/`.
Modules: `main`, `monitor_service`, and the components `component` (base),
`ble`, `lpc_device`, `modem`.

> The ELF imported almost entirely undisassembled; the gaps were flow-recovered
> ([[subecu-app-in-undisassembled-gap]]). The per-component bodies (ble/lpc/modem)
> are **inlined into the supervisor under `-O2`** and entangled with vendor
> mosquitto + nlohmann-json glue — they are not separate string-anchored
> functions, so the component layer is documented by behaviour + fingerprints
> rather than 1:1 functions.

`common_logf` is named; `main` (`0x109a98`) builds the service config, logs
`MonitorService starting`/`started`/`shut down`, constructs `MonitorService`
(`0x100`-byte heap object) and runs it.

## MonitorService (the supervisor)

ctor `monitor_service_ctor` (`0x120ac0`, vtable `0x170fd0`). It owns:
- a **component registry** `std::vector` of 64-byte elements (`obj+0x13..0x15`),
- the **MQTT/transport** (`obj+0x16`), an enable flag (`obj+0xc0`),
- a **runner/executor** sub-object (`obj+0xf8`) whose vtable provides the
  `should-run` / `supervise` / `start` virtuals.

It registers a **heartbeat** subscription as a `std::function` on the transport
with a **2000 ms** (`0x7d0`) interval. `monitor_service_on_heartbeat` (`0x1208f0`)
logs `Unexpected device is giving heartbeats %d.` (`monitor_service.cpp:0x3e`) for
a heartbeat from an unregistered device id.

**Supervise loop:** `monitor_service_run` (`0x120fe0`) takes the run mutex
(`obj+0x60`) and starts the runner (`runner vtable+0x18`). The poll-tick virtuals
(`monitor_service_poll_tick` `0x120530`) query `should-run` (`runner vtable+0x28`)
gated by the enable flag, and delegate to `monitor_service_supervise_components`
(`0x1204c0`) — an indirect dispatch (`runner vtable+0x10`) that fans out to each
component's poll/version/status handler.

`monitor_print_component_status_table` (`0x1221f0`) renders the supervisor's
component status to `std::cout` (a `GLOBAL STATE` / `SYSTEM` banner);
`monitor_print_component_row` (`0x121ac0`) prints one component's row (presence,
heartbeat id, name, type, version).

## GPIO reset bank (`component.cpp` + `common/gpio.cpp`)

The genuinely reconstructable VanMoof component logic is the **sysfs GPIO reset**
machinery:
- `gpio_export_and_configure` (`0x138bd0`) — builds `/sys/class/gpio/gpio<N>`,
  exports the pin via `/sys/class/gpio/export`, writes `out`/`in` to
  `<gpio>/direction`. (`gpio.cpp:0x18` error log.)
- `gpio_set_value` (`0x139180`) — writes `1`/`0` to `<gpio>/value` (the
  reset-line assert/deassert).
- `gpio_write_file` (`0x139950`) — generic sysfs `ofstream` writer.
- `component_gpio_reset_line_ctor` (`0x134bf0`) — a single GPIO reset line.
- `component_reset_lines_ctor` (`0x120310`) — the **reset-line bank**: 4 reset
  lines on **GPIO pins 1, 0, 10, 11** (one per resettable sub-ECU).
- `component_mqtt_client_ctor` (`0x13a8d0`) — the mosquitto-connected `IComponent`
  base; `component_format_version_string` (`0x1371e0`) formats `maj.min.patch`.

## The components (fingerprinted)

| Component | Fingerprint | Notes |
| --- | --- | --- |
| **BLE** (`ble.cpp`) | type tag `3BLE`; version from `ble/system/version_info` | the BLE chip; version queried over MQTT |
| **LPC device** (`lpc_device.cpp`) | `device/motor_control/version/{firmware,bootloader,vendor}`, `device/motor/status` | the LPC Cortex-M sub-ECUs; versioned over the bus |
| **modem** (`modem.cpp`) | `ping command` / `Ping successful` / `Ping failed`; `modem_nordic_stack`, `mfw_nrf9160_1.3.1` | **Nordic nRF9160** modem; aliveness is a **ping** (not a CAN poll) |

## MQTT topics

| Direction | Topics |
| --- | --- |
| publishes | `device/motor/status`, `device/motor_control/version/{bootloader,firmware,vendor}`, `monitor/component/charger/type`, component status |
| subscribes | `power/state{,/status,/extend_timeout}`, per-component heartbeats, `ble/system/version_info` |

## Status

22 app functions named (`main/monitor/ghidra/exports/monitor_program.json`). The
supervisor lifecycle, heartbeat routing, supervise scheduling, the GPIO reset
bank and the status-table renderer are mapped. Open: the per-component
version/aliveness method bodies are inlined under `-O2` (documented by behaviour +
fingerprints, not 1:1 functions); a deeper carve could separate them.
