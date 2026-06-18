# `power` service — Ghidra reconstruction

Reverse-engineering notes for `/usr/bin/power` (the battery/charger/power-state
daemon, pkg `vmxs5-embedded-power`). AArch64 C++, **stripped**, 512 functions;
imported in Ghidra as `/S5-v1.5/OS/power` (image base `0x100000`). MQTT user
**`power-service`**.

> Status: **substantially reversed.** The spine, object model, OD/telemetry
> table, the full **`vm` CAN wire protocol** (verified), the `StateManager` +
> all 7 state handlers, and the LiPo/charger control are documented; the
> **CAN/transport core is reconstructed to compiling C** under [`src/`](src/)
> (below). Method: the binary is stripped, but every log call is
> `common_logf(file, line, level, msg)`, so the embedded `*.cpp:line` + message
> strings reconstruct behavior cheaply; the decompiler resolves string literals
> inline. (Workflow notes for these C++ services live in the project memory.)

## Reconstructed source (`src/`, `include/`)

The **CAN/transport core** is reconstructed to faithful C (the self-contained,
verified VanMoof algorithms — not the STL/`vm`/mosquitto framework glue, which
stays as prose). Each TU compiles clean with `-Wall -Wextra -Wpedantic` (`make`);
like the other targets it is **behaviour-oriented, per-TU-compilable**, not a
linkable rebuild. Each function carries its OEM address.

| File | OEM | Contents |
| --- | --- | --- |
| [`include/vm_can.h`](include/vm_can.h) · [`src/vm_can.c`](src/vm_can.c) | `0x158d30`/`b60`/`c30` | the SocketCAN transport + the **verified** 29-bit `vm_address`↔CAN-ID bit-packing (`vm_can_open`/`tx`/`rx`). Real, standard SocketCAN C. |
| [`include/od_table.h`](include/od_table.h) · [`src/od_table.c`](src/od_table.c) | `0x158260`/`860`/`670` | the OD descriptor + 2-byte-key comparator + linear-scan find/add + the register-thunk pattern. |
| [`include/battery_decode.h`](include/battery_decode.h) · [`src/battery_decode.c`](src/battery_decode.c) | `0x1285c0`…`0x12da90`, `0x12cf80` | the per-signal battery payload decoders (voltage/charging/health/capacity/temperature) + the **SoC display-remap curve** `soc_to_soc_app`. |

Not reconstructed (vendor/framework or no clean numeric form): the StateManager /
state handlers / lipo_control (documented as prose below), the `status`/`warning`
bit-field decoders (~40/~20 runtime-named booleans), and the STL/`vm`/mosquitto
plumbing. See [`../docs/can-bus.md`](../docs/can-bus.md) for the full protocol.

## Source-module map (from embedded paths)

```
devices/main/power/src/   main · power_service · state_manager · power_control
                          lipo_control · low_power · switch_control · monitor
                          rtc_handler · wake_on_motion_handler · eshifter_calibration
devices/main/common/src/  service_env · mqtt_client · clock · gpio · switch_i2c · system_config
lib/src/common/           od_registration   (CANopen Object-Dictionary glue)
```

## Spine (named in Ghidra)

| Addr | Name | Role |
| --- | --- | --- |
| `0x10a220` | `main` | `ServiceEnv("power-service")` → build MQTT/clock/timer/config → `new PowerService(0x448)` → ctor → Run → Stop |
| `0x116350` | `PowerService_ctor` | builds sub-objects + registers the subscription/timer table (below) |
| `0x116e50` | `PowerService_Run` | starts the state client, runs, then shuts down |
| `0x116e10` | `PowerService_Stop` | shutdown |
| `0x129520` | `Monitor_ctor` | `Monitor` (`monitor.cpp`) — builds the **OD telemetry table** (primary battery + charger) and subscribes `device/charger/*` |
| `0x158f60` | `common_logf` | `(file, line, level, msg, …)` — the logger (everywhere) |

`PowerService` is a `0x448`-byte object. Sub-objects built by the ctor include
the **`Monitor`** (at `+0x2a`, the OD/telemetry registrar — see below), a
`StateManager` state machine, a `low_power` timeout member, `lipo_control` /
`power_control`, plus the `common::StateClient` base.

## MQTT + timer wiring (registered in the ctor)

**Subscribes** (handler reacts on the bus):

| Topic | Purpose |
| --- | --- |
| `power/state/set` | external state request → `StateManager::OnStateRequest` |
| `power/state/extend_timeout`, `power/low_power_extend` | extend the current state's timeout (`Extend state timeout with: %d ms`) |
| `maintenance/battery/primary/reset` | reset the primary (Panasonic) pack |
| `modem/system/time` | set the RTC from network time (only when in standby / transition) |
| `modem/vars/update` | modem variable sync |
| `device/charger/{connected,voltage,current,mode,finished}` | charger status from the external (LiteON) charger via the bus |

**Publishes:**

| Topic(s) | Content |
| --- | --- |
| `power/state`, `power/state/status` | current power state |
| `power/deep_sleep`, `power/low_power` | sleep / low-power notifications |
| `power/battery/lipo/info/*` | internal LiPo: `soc`, `voltage`, `current_now`, `temp`, `capacity`, `capacity_lvl`, `charge_full[_design]`, `cycles`, `health`, `pwr_avg`, `status` |
| `power/battery/primary/info/*` | Panasonic pack: `soc`, `soc_app`, `voltage`, `temperature`, `charge_current`, `discharge_current`, `max_current`, `power`, `cycles`, `health` |

**Timers** (`ITimerFactory`): a **4000 ms** periodic (telemetry publish) and a
**50 ms** periodic (fast state/charger-signal poll).

## The state machine (`state_manager.cpp`)

Reversed and named in Ghidra:

| Addr | Name | Role |
| --- | --- | --- |
| `0x142670` | `StateManager_StateName` | state enum → name string |
| `0x11acb0` | `StateManager_ChangeState` | commit a transition: publish state, store it, reset timeout |
| `0x11b520` | `StateManager_OnStateRequest` | gate + dispatch a requested state |

**State enum** (from `StateManager_StateName`, with the `IStateTransitions`
vtable slot each one dispatches to):

| # | State | `IStateTransitions` slot |
| --- | --- | --- |
| 0 | `INVALID` | — (rejected) |
| 1 | `SHIPPING` | vtable `+0x10` |
| 2 | `STANDBY` | `+0x18` |
| 3 | `OPERATIONAL` | `+0x20` |
| 4 | `CHARGING` | `+0x28` |
| 5 | `UPDATING` | `+0x30` |
| 6 | `ALARM` | `+0x38` |
| 7 | `MAINTENANCE` | `+0x40` |

**`OnStateRequest(req, force)`** (called from `power/state/set`) gates the
request, publishing the verdict on `power/state` (codes 0–3) and ending in
`OK`:

- current state still `0` ⇒ **DENIED** (not yet initialised);
- `req == current` & not `force` ⇒ **IS_ACTIVE**;
- `req != current` & a transition guard (`vtable+0x28` on the transitions obj)
  says busy & not `force` ⇒ **RETRY**;
- otherwise dispatch: call the `IStateTransitions` slot for `req` (skipped if it
  still points at the default `UNK_0011c4x0` stub), set current = `req`,
  log **OK**. Unknown `req` ⇒ *"Unhandled or invalid state request."*. `force`
  also arms a **12 000 ms** (`0x2ee0`) guard timer.

**`ChangeState(state)`** commits: rejects `0` (*"Someone's trying to go into
INVALID state"*), **publishes the state name on `power/state` and
`power/state/status`**, stores it at `this+0x40`, and clears the timeout counter
at `this+0x44`. Each state also drives the hardware switches via
`switch_control` (*"Set switches for state %s"* → GPIO / `switch_i2c`).

Transitions (from the log messages):

- **→ CHARGING:** charger connected while in standby (*"Charger connected while
  in standby"*, *"…not charging. Go to charging state"*). LiPo buck control:
  *"Enable Buck for charging lipo"*; aborts on *"No PGOOD received while charging
  the bike, turning the buck OFF"*.
- **→ STANDBY:** *"Charging finished. Go to standby state"*; *"Maximum lipo
  charge duration is reached, going back to standby"*; *"Primary Battery Reset
  completed — going to Standby"*.
- **→ SHIPPING:** *"Can't charge LiPo, going to shipping"*, *"Battery is off,
  going to shipping"*, *"Standalone main ECU, going to shipping"*; entering it
  logs *"Entering shipping mode"* (cf. `shipping_mode.sh` in
  [`../docs/update.md`](../docs/update.md) for the ship-mode IC sequence).
- **low-power / deep-sleep** (a timed sub-behaviour, not one of the 8 enum
  states; `power/low_power`, `power/deep_sleep`, extendable timeout): *"Go to
  sleep"* (guard *"Battery should be off before we go to sleep"*).
- **Standby coordination:** *"Send standby request to battery, LPC and nRF
  devices"* — i.e. the power service tells the **LPC55** and the **nRF** parts
  (BLE/modem) to go to standby too (more evidence the `lpc55sxx` is a live
  co-processor, see [`../docs/hardware.md`](../docs/hardware.md)).

## Power-state handlers (`IStateTransitions`)

`PowerService` overrides **all 7** state slots in its primary vtable
`&DAT_00184a88` (slots `+0x10`…`+0x40`, states 1–7) — independently confirmed via
`readelf -r`; **none** are the `UNK_0011c4x0` default stubs. Every handler takes
the recursive PowerService mutex (`this+0xe8`) on entry. Many just *record* the
state and re-arm/stop timers; the heavy work runs in the 4000 ms periodic worker.

Shared helpers: **`FUN_00112c40` "Turn on"** — sends CAN `power/low_power`
(`FUN_00125030` on `this+0x3a0`), requests CAN low-power mode (`FUN_00123940`),
clears substate flags, starts the 50 ms timer, asserts battery ON
(`FUN_001335d0`). Timer objects: `this+0x1f0` = 4000 ms (`+0x20(ms)` re-arm,
`+0x10` stop); `this+0x200` = 50 ms (`+0x18` start, `+0x10` stop).

| State | Handler | What entering it does |
| --- | --- | --- |
| 1 SHIPPING | `0x112720` | `ChangeState(1)`; re-arms the tick to 3000 ms. Actual shipping work (switch state 1, battery off, VAC/VBUS check, "Entering shipping mode") runs in the 4000 ms worker. |
| 2 STANDBY | `0x112980` | "To standby state"; clears substate `+0x120`/LiPo-counter `+0x394`; `ChangeState(2)`; re-arms to 3000 ms; `system("touch …/timesync/clock ; sync")` (RTC refresh). |
| 3 OPERATIONAL | `0x112d30` | **Guards**: if charger connected → ERROR "Charger connected. Not allowed to power on the system" + abort; if primary SoC == 0 → WARN "Primary Capacity is too low" + abort; else "Turn on". |
| 4 CHARGING | `0x1127c0` | "To charging state"; stops the tick; `FUN_0011fed0` resets the charging-session counters. Active buck/charge control runs in the worker. |
| 5 UPDATING | `0x112e40` | If current == CHARGING → `ChangeState(5)` (FOTA while charging, stay powered); else "Turn on" to bring the system up for the update. |
| 6 ALARM | `0x112f10` | Unconditionally "Turn on" — powers the bike up so the alarm/horn/lights are live. |
| 7 MAINTENANCE | `0x112440` | Stops **both** timers; if a battery is still ON → "Turning battery off…" + `FUN_00133730` (battery off); `switch_control` "Set switches for state 7" drives **all four** power switches active (gpiod); `ChangeState(7)`. |

## LiPo charge control (`lipo_control.cpp` + BQ25672)

HW: TI **BQ25672** buck/charger at I²C **2-006b** (sysfs
`/sys/bus/i2c/devices/2-006b/{registers,gpios}`); **bq27542** gauge at **2-0055**.

- **Buck enable/disable** (`0x11d920`, `switch_control.cpp:0x52`) toggles a
  **GPIO** line (not a register): `en=1` → drive high (buck on), `en=0` → low.
- **PGOOD-gated enable with retry** (`0x1136e0`): up to **5 attempts** — enable
  buck ("Enable Buck for charging lipo"), wait 100 ms, read PGOOD; on fail
  ("No PGOOD received while charging the bike, turning the buck OFF") disable +
  retry; after 5 fails → `ChangeState(STANDBY)`.
- **BQ25672 register writes** (to `2-006b/registers`, list `{01,02,0A}`): two
  profiles — *shipping* (`0x1220e0`) writes reg `02=0x7A, 0A=0x20`; *standby*
  (`0x122290`) writes reg `02=0xAE, 0A=0x23` (reg `01=0x01` both).
- **Status reads**: PGOOD (`2-006b/gpios` line `pwr-good`), input-mux
  (`tps2121st` — TPS2121), VAC1 (reg `CHRG_STAT_0` bit), gauge capacity/current/
  health.
- **`device/charger/*` reaction** (external LiteON charger over the bus): publish
  at `0x128c20` ("Charger connected"); `charger_decode_mode` → 0/1/2/3; **mode 2**
  → power off, **mode 3** → `ChangeState(STANDBY)` ("Charging finished").
- **Timers**: **4000 ms** (`FUN_00115a10`→`FUN_00115140`) = charge supervisor /
  telemetry tick (owns the buck/PGOOD/shipping/standby/charging sequences;
  re-arms 1000/3000/10000/15000/30000/900000 ms by sub-state). **50 ms**
  (`FUN_001146d0`) = fast poll / state publisher.

## Primary (Panasonic) battery — CAN / Object Dictionary

The bike does **not** use a raw DBC-style CAN signal table. The primary
(Panasonic) battery is modelled as a **CANopen-style Object Dictionary**
(`OdRegistration`, `lib/src/common/od_registration.cpp`), built in `Monitor_ctor`
(`0x129520`). Each battery datum is registered by **name + typed payload struct +
a decode→publish handler** (`Monitor_ctor` builds a vector of these):

| OD entry name | payload struct | → publishes |
| --- | --- | --- |
| `battery_primary_battery_status` | `battery_statusm` | (state) |
| `battery_primary_battery_charging` | `battery_chargingm` | charge flags |
| `battery_primary_battery_health` | `battery_healthm` | `…/info/health` |
| `battery_primary_battery_capacity` | `battery_capacitym` | `…/info/cycles`, `max_current`, … |
| `battery_primary_battery_temperature` | `battery_temperaturem` | `…/info/temperature` |
| `battery_primary_battery_voltage` | `battery_voltagem` | `…/info/voltage`, `current` |
| `battery_primary_battery_warning` | `battery_warningm` | (warning bits) |
| `battery_primary_battery_cell` | `battery_cellm` | per-cell |
| `battery_primary_battery_state_init` | — | init handshake |

Each entry is decoded by a per-signal lambda (`battery_*_decode`) and its
numeric CAN address **is** a static compile-time constant (one register thunk per
signal) — the full map (battery node `0xA4`, per-signal `a1`, computed CAN IDs)
is in **[`../docs/can-bus.md`](../docs/can-bus.md)**, §4.

The only **hardcoded raw CAN frames** in the binary are two **charger** commands
(*"Clearing test & burn-in mode of charger"*, `power_control.cpp:0xca`), sent via
`system("cansend vcan0 14E232{10,14}#A55A00")` — node `0xA7/0x11`, magic `A5 5A`.
The `device/charger/{connected,voltage,current,mode,finished}` MQTT signals (the
external LiteON charger via the bus) are wired up in `Monitor_ctor`.

**→ The full wire protocol is now decoded** (transport, 29-bit ID encoding, the
static OD address table, and the per-signal CAN IDs) and **adversarially
verified** — see **[`../docs/can-bus.md`](../docs/can-bus.md)**. Headline: the
`vm` library packs a 4-field `vm_address {a0,a1,a2,a3:5}` into the 29-bit
extended ID as `id=(a0<<21)|(a1<<13)|(a2<<5)|(a3&0x1F)|EFF`; the Panasonic pack
is node **`a0=0xA4`** (e.g. voltage = `0x14811040`, status = `0x1480F040`), the
charger is node **`a0=0xA7,a1=0x11`**. Names applied in Ghidra; map exported to
[`ghidra/exports/power_program.json`](ghidra/exports/power_program.json).

## Hardware interface (recap, see [`../docs/hardware.md`](../docs/hardware.md))

- **LiPo (internal):** fuel gauge **bq27542** + buck charger **BQ25672** on I²C
  bus 2 (`switch_i2c.cpp` / `sysfs_utils.h`); the `power/battery/lipo/info/*`
  metrics come from `/sys/class/power_supply/bq27542-0`.
- **Primary (Panasonic):** over **CAN**; `power/battery/primary/info/*`,
  `maintenance/battery/primary/reset`.
- **External charger (LiteON):** status arrives on `device/charger/*` (via the
  CAN/MQTT bridge), not direct I²C.
- **RTC** (`rtc_handler`, `/dev/rtc0`), **IMU wake-on-motion**
  (`wake_on_motion_handler`), power **switches** (`switch_control` + GPIO),
  **e-shifter calibration** during charging (`eshifter_calibration`).

## Next (when continuing)

- [x] `StateManager` gate + dispatch + state enum — done.
- [x] The **`IStateTransitions` per-state handlers** (7 states) → per-state actions — done (above).
- [x] `lipo_control` buck/charge logic (BQ25672) + `device/charger/*` reaction — done (above).
- [x] The 50 ms / 4000 ms timer handlers — done (above).
- [x] The **`vm` CAN wire protocol** + per-signal numeric CAN IDs — done & verified → [`../docs/can-bus.md`](../docs/can-bus.md).
- [ ] The 4000 ms periodic worker `FUN_00115140` internals (the shipping/sleep/
      `SuspendSystem` sub-state machine keyed on `this+0x120`).
- [ ] Cross-reference the CAN node map against the other ECU targets (see
      `can-bus.md` §6).

## Ghidra

Renamed functions / analysis state for this target are tracked here; the running
function map is in [`ghidra/exports/`](ghidra/exports/) (kept in sync as
functions get named, per the repo convention).
