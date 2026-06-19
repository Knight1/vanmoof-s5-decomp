# `update` service — Ghidra reconstruction

Reverse-engineering notes for `/usr/bin/update` (the peripheral-OTA orchestrator,
pkg `vmxs5-embedded-update`). AArch64 C++, **1951 functions**; imported in Ghidra
as `/S5-v1.5/OS/update` (image base `0x100000`). Unlike `power` this binary
**retains RTTI** (the C++ class names survive as `typeinfo` strings), so the class
hierarchy is recoverable directly; functions are still `FUN_*` and are named from
the `common_logf` anchor (`0x16c9a0`, every call embeds
`devices/main/update/src/<file>.cpp:line`).

> Status: **supplier-dispatch decoded** + the core VanMoof algorithms
> **reconstructed to compiling C** (`src/`, below). High-level OTA flow,
> manifest, two-stage/rollback, MQTT topics and the i.MX8 self-update are in
> **[`../docs/update.md`](../docs/update.md)**; this file holds the *decompiled*
> internals.

## Reconstructed source (`src/`, `include/`)

The self-contained VanMoof *algorithms* are reconstructed to faithful,
behaviour-oriented C (per-TU compilable, **not** a linkable rebuild — same bar as
[`../power/src`](../power/src)). The std::string / nlohmann-json / vm-CAN /
mosquitto framework is *modelled* per-TU (each module declares its own externs +
struct layouts with OEM offsets); only the VanMoof logic is translated. All **9
modules compile clean** with `-Wall -Wextra -Wpedantic` (`make`). Each function
carries its OEM address.

| Module | OEM | Contents |
| --- | --- | --- |
| [`manifest.c`](src/manifest.c) | `0x119210/118920` | `manifest.txt` CSV parse → `ManifestEntry`; semver pack `(variant<<13)\|patch\|(minor<<16)\|(major<<24)`; `allowed_to_skip`/`skip_rollback`. |
| [`update_client_factory.c`](src/update_client_factory.c) | `0x123220/12fbc0` | the **supplier dispatch**: name-token → client (`motor_control`/`_panasonic`/`_dynapack`/`charger`), the Panasonic version gate, the device-name→CAN-node parser. |
| [`thirdparty_update_client.c`](src/thirdparty_update_client.c) | `0x139000/1383e0/137790`, `0x13b6a0/620/750` | **the page-CRC-over-CAN flash FSM** (Panasonic/Dynapack/Liteon) — states SIZE_INFO/PAGE_CRC/IMAGE_CRC/UPDATE_REQ/TERMINAL, per-page CRC32 w/ 3 retries + full-image CRC, page geometry, per-vendor timing. Verified verbatim vs machine code. |
| [`lightweight_update_client.c`](src/lightweight_update_client.c) | `0x133370/131850`, `0x132250/132550` | the trimmed **CAN page-flash** client (lib targets, e.g. `MOTOR_CONTROL`) — OD ops `flash`/`ErasePage`/`write`/`reboot`, the page-CRC FSM (no charger validation). |
| [`c2000_update_client.c`](src/c2000_update_client.c) | `0x130bd0/130320/130580` | TI **TMS320F40049C** motor flash: GPIO reset, auto-baud, SCI-kernel byte-stream + echo verify, DFU app image (running checksum, footer magic `0x1be4`/`0xe41b`, status `0x1000`). |
| [`smp_modem_update_client.c`](src/smp_modem_update_client.c) | `0x120320/1f370`, `0x1f2e0/1e600/1e690` | nRF9160 **modem** update over **SMP/MCUmgr**: stop PPP → clear `modem/nordic/version_info` → disconnect LTE → SMP transfer (600 s watchdog, `ps`+`kill`) → restart `spi-mqtt-bridge@modem`. |
| [`background_update.c`](src/background_update.c) | `0x114ed0/113850/1120e0` | the **retry-table worker**: add/trigger/remove devices, the SoC-gated (≥11 %) worker loop, result→string map, `update/background_update/<dev>`. |
| [`version_client_mqtt.c`](src/version_client_mqtt.c) | `0x12e350/12e670/12e8f0` | the per-device **reported-version store** (`device/+/version/*`) + the u32/u16/u8 version-string parsers. |
| [`runfota.c`](src/runfota.c) | `0x1d220/1dc80/1b750` | the i.MX8 A/B **boot-control** logic: parse `/tmp/boot_control_flag` (`su_<ver>_<SP>`), `fw_setenv su_state`, FOTA-header version scan. (Install steps are thin `system()` wrappers — shell commands kept verbatim.) |

**Shared:** [`include/update_common.h`](include/update_common.h) (logger, enums,
crc/json/string/serial model) + [`Makefile`](Makefile). Vendor framework
(`mqtt_client`, `service_env`, `state_client`, `tp`, STL/json) is **not**
reconstructed — modelled only.

**Documented-only:** the `update_service.cpp` orchestrator **spine** (the 2-stage
A/B + rollback state machine, MQTT/thread glue) is mapped function-by-function
(below + `ghidra/exports/`) but kept as prose — its C++ object embeds modelled
strings by value with offset-critical layout, so it isn't cleanly per-TU
modelable; the *logic* is captured in the state-machine description and the
function map.

## Headline: how the bike picks Panasonic vs DynaPack (supplier dispatch)

This closes the question left open in [`../power/README.md`](../power/README.md):
the `power` service never senses the battery supplier — **the `update` service
decides it, purely from the OTA package's file/device name.**

`UpdateClientFactory::GetUpdateClient(name)` (`0x123220`) is a token-matching
chain (`std::string::find`) that constructs the right `IUpdateClient` subclass:

| Name contains | Client built | Node | Notes |
| --- | --- | --- | --- |
| `_panasonic` | **`PanasonicUpdateClient`** (`0x13b6a0`) | battery `0xA4` | **version-gated** (see below) |
| `_dynapack` | **`DynapackUpdateClient`** (`0x13b620`) | battery `0xA4` | different protocol/timing |
| `charger` | **`LiteonUpdateClient`** (`0x13b750`) | charger `0xA7` | registers a `charger_status` OD handler |
| *(token code → modem)* | `SmpModemUpdateClient` (`0x13f540`) | nRF/modem | SMP/MCUmgr |
| *(token code → motor)* | `C2000UpdateClient` (`0x133370`) | TI C2000 | 15 s/2 s/180 s timing |
| `battery_primary` w/ neither supplier | — | — | **throws** *"…first token of file name should be battery_primary_panasonic or battery_primary_dynapack"* |

So **Panasonic and DynaPack are genuinely different flashing implementations**
(both subclass `ThirdPartyUpdateClient`, but with different page/timeout
constants and protocol modes) — not one client parameterised by a sensed
chemistry byte. Supplier selection is a build-/package-time fact carried in the
firmware **filename** (`battery_primary_panasonic` / `battery_primary_dynapack`),
matched here against the manifest entry.

### Panasonic version gate

`PanasonicUpdateClient_ctor` takes a `bool` set by the factory from
`version > 0x010300ff` (**v1.3.0.255**). It selects two timing profiles:

| | page timeout | step | retry | full-image timeout |
| --- | --- | --- | --- | --- |
| **legacy** (≤ v1.3.0.x) | 300 ms | 30 ms | 30 ms | 900 s |
| **new** (> v1.3.0.x) | 100 ms | 10 ms | 10 ms | 360 s |

i.e. newer Panasonic BMS firmware flashes ~3× faster. `DynapackUpdateClient` is
fixed at 50 ms / 1 ms (mode 0); `LiteonUpdateClient` at 20 ms / 10 ms / 10 ms.

## Update-client class hierarchy (from RTTI)

```
IUpdateClient (interface)
├── LightweightUpdateClient        lightweight_update_client.cpp   (CAN page-flash base)
├── C2000UpdateClient              c2000_update_client.cpp         (TI motor controller)
├── SmpModemUpdateClient           smp_modem_update_client.cpp     (nRF modem/BLE via SMP/MCUmgr)
├── MqttUpdateClient               mqtt_update_client.cpp          (transport over MQTT)
└── ThirdPartyUpdateClient         thirdparty_update_client.cpp    (battery/charger base)
    ├── PanasonicUpdateClient      battery 0xA4, version-gated
    ├── DynapackUpdateClient       battery 0xA4
    └── LiteonUpdateClient         charger 0xA7

UpdateClientFactory  ── creates one of the above from the device/file name token
BackgroundUpdate     ── the multi-stage (UpdateStage) driver
Manifest             ── manifest.txt parser
VersionClientMqtt    ── per-device running-version query over MQTT
```

Source modules (embedded `devices/main/update/src/` paths, from `common_logf`):
`main`, `update_service`, `background_update`, `manifest`, `runfota`,
`version_client_mqtt`, `lightweight_update_client`, `thirdparty_update_client`,
`smp_modem_update_client`, `c2000_update_client`, `mqtt_update_client`.

## Function map

A `common_logf`-anchored fan-out mapped all **99 logging functions** (~60 in the
`update` app, ~39 in shared `common`/`lib`). The full per-function table —
address, module, role, log strings — is in
[`ghidra/exports/update_program.json`](ghidra/exports/update_program.json). The
behaviour by module:

### Boot/main + service spine
- **`main`** `update_service_main` (`0x10c0e0`): parses `-f/--force`, `-e/--exit`
  + the positional FOTA dir, opens the CAN device `/dev/ttymxc3`, builds the
  whole `UpdateService` graph (SMP transport, MQTT, manifest, runfota,
  background_update, version_client), runs, tears down. Download path
  `/tmp/download/VM_XS5_FOTA`.
- **`update_service`** — the orchestrator + state machine (below): publishes
  `update/current_version`, watches the manifest topic, drives per-device
  updates and the i.MX8 self-update, and reports progress.

### The i.MX8 self-update state machine (`update_service` + `runfota`)
`update_service_run_state_machine` (`0x12a6d0`) is the **two-stage A/B install
with rollback**, gated on battery level (*"Battery is to low to start update"*):

- queries the boot-control client for the current stage (`runfota_read_boot_control_flag`,
  `0x1d220` — *"Boot control version: %s, partition: %d, stage: %d"*);
- **fresh start** → `update_service_run_fota_install_sequence` (`0x129600`):
  `runfota` shells out step-by-step — split FOTA file → mount root → extract
  scripts → umount → pre-install → install → post-install → **reboot**
  (each step `runfota_*` at `0x1b000…0x1b480`, all `system()` calls; progress
  event after each);
- **Stage 1** → set boot command flag for the inactive partition
  (`runfota_set_boot_command_flag`, `0x1dc80`, `fw_setenv`) → execute reboot;
- **Stage 2** → install devices, then **commit** (boot-control vtable `+0x70`)
  or, on failure, **rollback** (`+0x78`) + reboot (*"Failed to install devices
  in update 'Stage 2', rolling back."*);
- a force-rollback MQTT message (`update_service_on_force_rollback_message`,
  `0x12ae00`) forces boot flag = 2 → rollback → reboot.

`runfota` also supports **xdelta parked-delta** flow (`park`/`restore`/`exists`/
`delete` parked delta, `0x1c340…0x1d000`) for delta FOTA images.

### Per-device peripheral updates
`update_service_run_device_updates` (`0x129c90`) walks the manifest device list:
skip (rollback mode / `skip_rollback` flag), or update each via
`update_service_handle_device_update` (`0x125040`) — compare reported vs wanted
version (3 retries; force if no version reported), get the client from the
factory, flash (3 attempts). Failures that are **allowed-to-skip** get queued
into the **background-update** table (`background_update_*`, `0x1114c0…0x114ed0`)
— a worker that retries later once **battery SoC ≥ 11 %** and the bike is idle,
publishing `update/background_update/<device>`.

### Per-target flash protocols (the `IUpdateClient` subclasses)
| Client | Target | Protocol (decoded) |
| --- | --- | --- |
| `ThirdPartyUpdateClient` (Panasonic/Dynapack/Liteon) | battery `0xA4` / charger `0xA7` | **page-CRC over CAN**: `on_flash_size_info` (`0x1383e0`) byteswaps page size/count + bounds-checks; `on_crc_response` (`0x139000`) does per-page CRC with **3 retries** (*"CRC check of a page failed 3 times"*) then a full-image CRC (*"CRC over full image"* / *"CRC check of the entire image failed"*), emitting *"Progress: %d %%"*; Liteon also validates *"charger type is matched with max current"* (`0x138d80`). |
| `LightweightUpdateClient` (`0x133370`) | CAN devices (e.g. motor_control) | trimmed CAN flash; OD ops `flash` / `ErasePage` / `write` / `reboot`. |
| `C2000UpdateClient` | TI **TMS320F40049C** motor controller | `c2000_flash_motor_controller` (`0x130bd0`): GPIO-reset (`/sys/class/gpio` gpio12/125/126) → auto-baud → SCI-kernel byte-stream w/ echo verify (`0x130320`) → **DFU** application image w/ running checksum + footer magic `0x1be4` (`0x130580`). |
| `SmpModemUpdateClient` | nRF9160 **modem** (& BLE) | **SMP/MCUmgr** over serial: stop PPP → clear `modem/nordic/version_info` → disconnect LTE → `smp_modem_system_execute_with_timeout` (`0x1f370`, 600 s watchdog, `ps|grep`+`kill`) runs the SMP transfer → restart `spi-mqtt-bridge@modem`. |
| `MqttUpdateClient` / `mqtt_ftp_client` | devices reachable over MQTT | JSON-over-MQTT FTP RPC (`op` 2/5): `start_transfer` → `transfer_chunk` (≤0x2000, running CRC) → `transfer_finish` (`0x1408a0…0x142250`); `send_file` driver at `0x1433d0`. |

### MQTT (decoder-confirmed, complements [`../docs/update.md`](../docs/update.md))
Publishes `update/current_version`, `update/progress`, `update/finished`,
`update/ux/finished`, `update/update_available`, `update/background_update/<dev>`,
`power/low_power_extend` (keep-awake during flashing). Subscribes the manifest /
version / background-update / force-rollback topics + (via `common`) `power/state*`.

## Named in Ghidra

**All 104** identified functions are named + saved: `common_logf`, the factory,
the client ctors, and the 99 mapped `common_logf` callers (~60 `update`-app +
~39 shared `common`/`lib`). Full table:
[`ghidra/exports/update_program.json`](ghidra/exports/update_program.json).

## Next

- [x] Supplier dispatch (Panasonic/DynaPack/Liteon) — **done**; closes the open
      item in `power/README.md`.
- [x] Per-function map of all `common_logf` callers — **done** (99 functions).
- [x] Two-stage + rollback state machine; per-client flash protocols — **mapped** (above).
- [x] Apply all ~104 names in Ghidra + save — **done**.
- [x] Reconstruct the core VanMoof algorithms to compiling C — **done**: the
      page-CRC flash FSM, C2000 DFU, supplier dispatch, manifest parse and the
      A/B boot-control logic.
- [x] `lightweight_update_client` + `smp_modem` transfer loops, the
      `background_update` retry worker, `version_client_mqtt` — **done** (`src/`
      now **9 modules**, clean `-Wall -Wextra -Wpedantic`).
- [ ] (optional) re-model the `update_service` spine to the `power`-style opaque
      `_raw[]`+accessor form to bring it into `src/` (currently prose).
