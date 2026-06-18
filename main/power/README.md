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

The VanMoof logic of the power service is reconstructed to faithful C. All **8
translation units compile clean** with `-Wall -Wextra -Wpedantic` (`make`); like
the other targets it is **behaviour-oriented, per-TU-compilable**, not a linkable
rebuild (the STL/`vm`/mosquitto framework is *modelled* via
[`include/power_common.h`](include/power_common.h), not byte-rebuilt). Each
function carries its OEM address.

| Module | OEM | Contents |
| --- | --- | --- |
| [`vm_can`](src/vm_can.c) | `0x158d30/b60/c30` | SocketCAN transport + the **verified** 29-bit `vm_address`↔CAN-ID bit-packing. |
| [`od_table`](src/od_table.c) | `0x158260/860/670` | OD descriptor + 2-byte-key comparator + scan/add + register-thunk pattern. |
| [`battery_decode`](src/battery_decode.c) | `0x1285c0…12da90`, `0x12cf80` | primary-battery payload decoders + the **SoC display-remap curve**. |
| [`power_control`](src/power_control.c) | `0x133590/138f90`, `0x133730…ac0` | the **battery/charger command set** (opcodes 0/1/5/6/8/9) + the `power_control_state` status callback (`IsPrimaryInserted`). |
| [`state_manager`](src/state_manager.c) | `0x11acb0/b520/142670` | `ChangeState` / `OnStateRequest` / `StateName` + the 8-state `IStateTransitions` dispatch. |
| [`lipo_control`](src/lipo_control.c) | `0x1136e0/11d920/12ba60` | the bq27542 gauge reads + BQ25672 buck/PGOOD control + register profiles + the PGOOD-retry / fault-recovery reset. |
| [`low_power`](src/low_power.c) | `0x123c30/124660/124860` | `SuspendSystem` (RTC/motion wake, `/sys/power`), `poweroff`, the CAN standby broadcast + CAN-quiet check. |
| [`switch_control`](src/switch_control.c) | `0x11e430/1477b0/148070` | the per-state power-switch matrix + the sysfs GPIO backend. |

**Shared:** [`power_common.h`](include/power_common.h) — the `battery_cmd`/
`power_state` enums, the `common_logf`/`od_pub_*`/`sysfs_*`/`gpio_*` framework
interfaces, and the `PowerService` (0x448) offset map.

Still prose-only (STL-heavy or no clean form): the `power_service.cpp`
spine/ctor/handlers, `monitor.cpp` wiring, the `status`/`warning` bit-field
decoders (~40/~20 runtime-named booleans), and `rtc_handler`/`wake_on_motion`/
`eshifter_calibration` (documented below). Full MQTT catalog:
**[`mqtt.md`](mqtt.md)**; full CAN protocol: [`../docs/can-bus.md`](../docs/can-bus.md).

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

## BMS: type detection, reset & command set

### Battery type: Panasonic vs DynaPack

Within the `power` service the bike does **not** decide Panasonic-vs-DynaPack
from any runtime signal — **there is no BMS-type detector in this binary.** The
device-name fragments `battery_primary` (VA `0x15c2e8`), `_panasonic`
(`0x15c2f8`), `_dynapack` (`0x15c308`), `_liteon_normal` (`0x15c318`),
`_liteon_speed` (`0x15c328`) are loaded only via `ADRP(0x15c000)+ADD` (no Ghidra
xref). An ADRP+ADD resolver over the ELF shows every genuine user is a **static
name-table builder** — `main` and the static ctors `_INIT_7` (`0x10cee0`),
`_INIT_11` (`0x1119d0`), `_INIT_12` (`0x111bd0`) — which concatenate the base +
each supplier suffix (`std::string` ctor `0x112100` + `operator+` `0x111fb0`)
and emit **all** names *unconditionally* into a global registry of
device-firmware-target names (`battery_primary_panasonic`,
`battery_primary_dynapack`, `charger_liteon_{normal,speed}`, `motor_control`, …).
At each site the only branch is the stack-canary epilogue — **no `cmp` on a
battery-type byte.**

So here Panasonic/DynaPack is purely a set of **FOTA / firmware-image
identifiers** (matched downstream against update payloads / version reporting),
not a sensed property. Battery telemetry comes from a local TI **bq27542** fuel
gauge over sysfs (`/sys/class/power_supply/bq27542-0/…`, `FUN_001214c0`) and the
CAN status/health decoders expose **no** vendor/manufacturer/model field (a
whole-binary scan finds `panasonic`/`dynapack`/`liteon` only as those fragments).
**The supplier-selection logic is not in this `power` service** — it lives in the
BMS firmware or another host service (the `update` service matches the reported
device version against the per-supplier firmware names). *(Confirming the exact
matcher is the natural next step — the `update` binary is already in Ghidra.)*

### Battery / charger CAN command set

All battery commands are single-byte opcodes stamped into frame byte `[0]` and
published to the **power-control board** (node `a0=0xA3`, `a1=0x01`; OD index
`0x1a3`, descriptor `0x8201a3`) — **not** to battery node `0xA4` directly; the
power-control board relays to the battery. Send path: `FUN_00133590` →
`FUN_00138f90` stamps the opcode → publish frame `obj+0x40` via the OD/TP client
`obj+0x90` (`FUN_00157ca0`, 3 retries / 100 ms). CAN id
`(0xA3<<21)|(0x01<<13)|(0x82<<5)|EFF ≈ 0x14603040`.

| cmd | meaning | wrapper | notes |
| --- | --- | --- | --- |
| `0` | **Battery OFF** | `FUN_00133730` | shipping / fully-charged / maintenance / charge-worker |
| `1` | **Battery ON** | `FUN_001335d0` | `PowerService_TurnOn`, fault recovery |
| `5` | **IdentifyCharger** | `FUN_00133660` | `power_control.cpp:0xd4`; first runs the charger clear-test |
| `6` | **Battery RESET** | `FUN_00133ac0` | `power_control.cpp:0xf0` *"Sending battery reset command"* |
| `8` | **Shipping mode** | `FUN_00133780` | final step after power-off (*"Entering shipping mode"*) |
| `9` | **Clear fault flags** | `FUN_001337d0` | *"Try to clear battery flags."* |

(`get_xrefs_to FUN_00133590` returns exactly these six sites; the `#5`/`#6`
opcodes are `strb`-verified in machine code.)

Separate **raw charger clear-test** (`power_control_clear_charger_test_burnin`
`0x133620`, `power_control.cpp:0xca`) — two `system()` `cansend` calls to the
**charger** node `0xA7`: `cansend vcan0 14E23214#A55A00` (set) /
`14E23210#A55A00` (clear), payload `A5 5A 00`.

### BMS reset flow

- **MQTT-triggered:** `maintenance/battery/primary/reset` → `FUN_00112a50`
  (*"MQTT Request to Reset Primary Battery."*) → cmd `6` → `nanosleep` ~16 s →
  *"Primary Battery Reset completed - going to Standby."* → `PowerService_OnStandby`.
- **Automatic fault recovery** (in `lipo_buck_enable_pgood`, gated on monitor
  flag `+0x121==1`, set when `charger_decode_mode` `monitor.cpp:0x244` returns
  mode 2 → **the feature string** *"Battery has generic fault bit set, need to
  reset battery"* `0x15ed28`):
  *"Try to clear battery flags"* → cmd `9` → ~2 s → cmd `1` (ON) → if still off
  → *"Unable to clear flags, try battery reset"* → cmd `6` (reset) → ~21 s → cmd
  `1` → *"Battery reset succesfull, battery on."* (else *"Unable to turn on
  battery."*, flag `+0x121=3`).

### Battery insertion detection (`BatteryINS-DET`)

Detected by **CAN status bits, not GPIO**. The log `IsPrimaryInserted: %d
BatteryINS-DET: %d BatteryINS-DET FAIL: %d` reads three bit-fields:

| field | slot | source bit |
| --- | --- | --- |
| `IsPrimaryInserted` | power_control `+0x49` (`FUN_00133450`) | power-control status `frame[2] & 1` |
| `BatteryINS-DET` | monitor `+0x8f` (`FUN_00128170`) | battery status `frame[6]>>5 & 1` |
| `BatteryINS-DET FAIL` | monitor `+0x90` (`FUN_00128190`) | battery status `frame[6]>>4 & 1` |

`frame[2]&1 == 0` → *"Primary battery not detected"*. No firmware debounce — FAIL
is a BMS-reported bit. (The sysfs GPIO class that exists drives the **buck/charger**
line, not insertion.)

## Charge-supervisor worker (4000 ms)

`charge_supervisor_worker` (`0x115140`, `power_service.cpp`) is the periodic
**Standby** sub-state machine, run by `timer_4000ms_charge_supervisor`
(`0x115a10`) while in/entering Standby. The sub-state byte is `this+0x120`; each
pass re-arms `this+0x1f0` with a state-specific period.

**Prelude** (every pass): if a charger connects in standby → *"Charger connected
while in standby"*, arm 30000 ms, return. Else if charger present + battery up →
`PowerService_TurnOn`, then *"Enable Buck for charging lipo"*
(`buck_set_enable(1)`, 100 ms), byte→`1` (stamp charge-start ts); *"LiPo
Capacity: %d Current: %d"*; if `cap==0 & Dead & I<1` → reset fuel gauge.

| byte | name *(tbc)* | action | re-arm |
| --- | --- | --- | --- |
| `0` | kStandbyIdle/restart | Standby switches; battery on if charger off; byte→`2` | 10000 ms |
| `1` | kStandbyLiPoCharging | charge monitoring (below) | 3000–30000 ms |
| `2` | kStandbyGoToSleep | if battery still ON → *"Battery should be off before we go to sleep"*, byte→`0` (3000); else *"Send standby request to battery, LPC and nRF devices"*, byte→`3` (1000) | 1000–10000 ms |
| `3` | kStandbyCheckCanQuiet | *"Check that there is no traffic on the CAN bus"* — read `/sys/class/net/vcan0/statistics/rx_packets` over 500 ms; quiet → byte→`4`; else retry ≤3 then restart | 1000 ms |
| `4` | kStandbySuspend | VAC2/VAC1/VBUS present → *"…Stay awake."* (30000); else *"Go to sleep"*: `SuspendSystem(0x708, …)` = SetWakeAlarm + EnableWakeOnMotion (RTC/motion wake) | 1000–30000 ms |

**Byte `1` detail:** elapsed ≥ 3600 s → *"Maximum lipo charge duration… going
back to standby"* (byte→0); standalone main ECU → *"Standalone main ECU, going to
shipping"* (`ChangeState(1)`, 900000 ms); not charging while ON → *"Battery is ON
but not charging LiPo"* (re-toggle buck, read `PGOOD/MuxSwitch/VAC1`); `cap<31` &
(`primary RSOC==0` or `LiPo CheckTimes==10`) → *"Can't charge LiPo, going to
shipping"*; else continue (30000 ms).

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
- [x] The 4000 ms charge-supervisor worker (sub-state machine) — done (above).
- [x] BMS type detection, reset + command set, insertion detect — done (above).
- [ ] Confirm the **supplier matcher** (Panasonic vs DynaPack) in the `update`
      binary (already in Ghidra) — it's not in `power`.
- [ ] Cross-reference the CAN node map against the other ECU targets (see
      `can-bus.md` §6).

## Ghidra

Renamed functions / analysis state for this target are tracked here; the running
function map is in [`ghidra/exports/`](ghidra/exports/) (kept in sync as
functions get named, per the repo convention).
