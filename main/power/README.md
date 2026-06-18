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

The VanMoof logic of the power service is reconstructed to faithful C —
**all 12 `.cpp` modules**, including the STL-heavy `power_service.cpp` spine and
`monitor.cpp` wiring (the `std::function`/`std::vector`/`std::string` plumbing is
*modelled* as data-driven subscription/timer tables + typed publish calls via
[`include/power_common.h`](include/power_common.h), not byte-rebuilt). All **12
translation units compile clean** with `-Wall -Wextra -Wpedantic` (`make`);
behaviour-oriented, per-TU-compilable, not a linkable rebuild. Each function
carries its OEM address.

| Module | OEM | Contents |
| --- | --- | --- |
| [`vm_can`](src/vm_can.c) | `0x158d30/b60/c30` | SocketCAN transport + the **verified** 29-bit `vm_address`↔CAN-ID bit-packing. |
| [`od_table`](src/od_table.c) | `0x158260/860/670` | OD descriptor + 2-byte-key comparator + scan/add + register-thunk pattern. |
| [`battery_decode`](src/battery_decode.c) | `0x1285c0…12da90`, `0x12cf80` | primary-battery payload decoders + the **SoC display-remap curve**. |
| [`power_control`](src/power_control.c) | `0x133590/138f90`, `0x133730…ac0` | the **battery/charger command set** (opcodes 0/1/5/6/8/9) + the `power_control_state` status callback. |
| [`state_manager`](src/state_manager.c) | `0x11acb0/b520/142670` | `ChangeState` / `OnStateRequest` / `StateName` + the 8-state dispatch. |
| [`lipo_control`](src/lipo_control.c) | `0x1136e0/11d920/12ba60` | bq27542 gauge + BQ25672 buck/PGOOD control + register profiles + the recovery reset. |
| [`low_power`](src/low_power.c) | `0x123c30/124660/124860` | `SuspendSystem` (RTC/motion wake, `/sys/power`), poweroff, CAN standby broadcast + CAN-quiet check. |
| [`switch_control`](src/switch_control.c) | `0x11e430/1477b0/148070` | the per-state power-switch matrix + the sysfs GPIO backend. |
| [`power_service`](src/power_service.c) | `0x10a220/116350`, `0x112440…f10` | the **spine**: `main`, the ctor's subscription/timer wiring, Run/Stop/TurnOn, the **7 state handlers**, the **5 MQTT handlers** + `on_mqtt_battery_reset`, and the **4000 ms charge-supervisor** sub-state machine. |
| [`monitor`](src/monitor.c) | `0x129520`, `0x1268d0/1275a0`, `0x128cc4` | the OD-registration wiring, `charger_decode_mode`, the `device/charger/*` publishers, and the `status`/`warning` **bit-field decoders** (~40/~20 flags). |
| [`battery_power_on`](src/battery_power_on.c) | `0x112560/112fc0`, `0x113130/113280/113580` | the **battery power-on / buck / charging supervisor** virtuals — recovered by hand (see below); were missed by Ghidra auto-analysis. |
| [`eshifter_calibration`](src/eshifter_calibration.c) | `0x11fee0/120030`, `0x120680/120a40/120ac0` | the **e-shifter auto-calibration FSM** (idle→request→sent→in-progress) + the >30-day rate-limiter, the retry/giveup watchdog, and the `eshifter/{state,gear/set,last_calibrated}` wiring. |

**Shared:** [`power_common.h`](include/power_common.h) + per-module headers model
the STL/`vm`/mosquitto framework (subscription/timer/sysfs/gpio interfaces, the
`battery_cmd`/`power_state` enums, the `PowerService` 0x448 offset map).

Thin OS-HAL wrappers, **decoded + named in Ghidra** but kept out of `src/` (they
are just ioctl/sysfs shims, not VanMoof algorithms): `rtc_handler` (`/dev/rtc0`
`RTC_RD_TIME` / `RTC_WKALM_RD` / `RTC_WKALM_SET`) and `wake_on_motion_handler`
(IMU detect via `imu-lib`, `wom_thr_*` / `event_wom_enable` sysfs). Full MQTT
catalog: **[`mqtt.md`](mqtt.md)**; full CAN protocol:
[`../docs/can-bus.md`](../docs/can-bus.md).

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

Shared helpers: **`PowerService_TurnOn` (`0x112c40`) "Turn on"** — sends CAN
`power/low_power` (`low_power_publish_low_power` `0x125030` on `this+0x3a0`),
requests CAN low-power mode (`low_power_request_can_low_power` `0x123940`),
clears substate flags, starts the 50 ms timer, asserts battery ON
(`battery_cmd1_on` `0x1335d0`). Timer objects: `this+0x1f0` = 4000 ms (`+0x20(ms)` re-arm,
`+0x10` stop); `this+0x200` = 50 ms (`+0x18` start, `+0x10` stop).

| State | Handler | What entering it does |
| --- | --- | --- |
| 1 SHIPPING | `0x112720` | `ChangeState(1)`; re-arms the tick to 3000 ms. Actual shipping work (switch state 1, battery off, VAC/VBUS check, "Entering shipping mode") runs in the 4000 ms worker. |
| 2 STANDBY | `0x112980` | "To standby state"; clears substate `+0x120`/LiPo-counter `+0x394`; `ChangeState(2)`; re-arms to 3000 ms; `system("touch …/timesync/clock ; sync")` (RTC refresh). |
| 3 OPERATIONAL | `0x112d30` | **Guards**: if charger connected → ERROR "Charger connected. Not allowed to power on the system" + abort; if primary SoC == 0 → WARN "Primary Capacity is too low" + abort; else "Turn on". |
| 4 CHARGING | `0x1127c0` | "To charging state"; stops the tick; `charge_counters_reset` (`0x11fed0`) resets the charging-session counters. Active buck/charge control runs in the worker. |
| 5 UPDATING | `0x112e40` | If current == CHARGING → `ChangeState(5)` (FOTA while charging, stay powered); else "Turn on" to bring the system up for the update. |
| 6 ALARM | `0x112f10` | Unconditionally "Turn on" — powers the bike up so the alarm/horn/lights are live. |
| 7 MAINTENANCE | `0x112440` | Stops **both** timers; if a battery is still ON → "Turning battery off…" + `battery_cmd0_off` (`0x133730`); `switch_control` "Set switches for state 7" drives **all four** power switches active (gpiod); `ChangeState(7)`. |

## Power-on / buck supervisor — recovered virtuals (`battery_power_on.c`)

The same primary vtable `&DAT_00184a88` holds **more** `power_service.cpp`
virtuals *after* the 7 state slots — the battery power-on / buck / charging
supervisors. These are reached **only** through the vtable (their entry
addresses live as raw 8-byte pointers built with `ADRP+ADD`-to-data, with no
string-anchored xref), so Ghidra's auto-analysis **never carved them**: a string
audit found *"Battery error!"* (`0x15c778`) and its neighbours apparently
unreferenced, which led to an uncarved 1.4 KB code region. Walking the vtable by
hand (slots `0x184af0`…`0x184b08`) recovered the cluster. Now carved, named,
plate-commented in Ghidra and reconstructed in
[`src/battery_power_on.c`](src/battery_power_on.c):

| Addr | Name | `*.cpp:line` | Role |
| --- | --- | --- | --- |
| `0x112560` | `battery_power_supervise` | — | buck/fault **watchdog**: state-gated buck force-on (*"BUCK ENABLED"*, `switch_control.cpp:0x52`); arms the timestamped fault flag (`+0x121`, ts at `+0x370`) for OPERATIONAL/UPDATING/ALARM requests. |
| `0x112fc0` | `battery_charging_poll` | `:0x29d` | charger-mode evaluator: mode 0→fault, 1→`IdentifyCharger`, 2→*"Battery is fully charged, powering off"* / one-shot reset, 3→battery off + →STANDBY. |
| `0x113130` | `battery_power_on_recovery` | `:0x335` | the **"Battery error!"** handler: latch fault, PGOOD-guarded buck pulse if charger present (*"…turning the buck OFF"* `:0x347`), then `battery_charging_poll`. |
| `0x113280` | `battery_power_on_sequence` | `:0x2ba–0x2ee` | power-on / charger-state supervisor: drives OPERATIONAL↔CHARGING↔STANDBY, owns *"In battery power on sequence recovery"*, *"Battery powered off in operational state…"*, *"Charger connected, but not charging. Go to charging state"*, *"Charging finished. Go to standby state"*. |
| `0x113580` | `battery_enter_charging` | `:0x322` | enter-charging: PGOOD-guarded buck (*"No PGOOD received while charging"*), switch state 4, CAN low-power, →CHARGING. |

Each `power_service.cpp` body is also exposed through a second base vtable (at
`this+0x50`), so the compiler emitted two 8-byte **adjustor thunks** (`0x113270`
→recovery, `0x113570`→sequence: `sub x0,x0,#0x50 ; b body`) — vendor artifacts,
not reconstructed. Two `PowerControl` accessors they call were *also* uncarved
(`power_control_buck_needed` `0x1332f0`, `power_control_retry_pending`
`0x133410`), as was the eshifter-on-charge trigger `eshifter_request_calibration`
(`0x120680`, `eshifter_calibration.cpp:0x68`, STL-heavy → prose only). All now
named + saved. *(Byproduct of the same vtable sweep: the StateManager virtuals
`StateManager_StateRequestTimeout` `0x11c2c0` and `StateManager_ExtendStateTimeout`
`0x11a760`, previously uncarved, were named too.)*

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
- **Timers**: **4000 ms** (`timer_4000ms_charge_supervisor` `0x115a10` →
  `charge_supervisor_worker` `0x115140`) = charge supervisor / telemetry tick
  (owns the buck/PGOOD/shipping/standby/charging sequences; re-arms
  1000/3000/10000/15000/30000/900000 ms by sub-state). **50 ms**
  (`timer_50ms_poll` `0x1146d0`) = fast poll / state publisher.

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
gauge over sysfs (`/sys/class/power_supply/bq27542-0/…`, `bq27542_read_charge_now`
`0x1214c0`) and the
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
power-control board relays to the battery. Send path: `battery_send_command`
(`0x133590`) → `battery_stamp_command` (`0x138f90`) stamps the opcode → publish
frame `obj+0x40` via the OD/TP client `obj+0x90` (`vm_tp_publish_retry` `0x157ca0`,
3 retries / 100 ms). CAN id
`(0xA3<<21)|(0x01<<13)|(0x82<<5)|EFF ≈ 0x14603040`.

| cmd | meaning | wrapper | notes |
| --- | --- | --- | --- |
| `0` | **Battery OFF** | `battery_cmd0_off` (`0x133730`) | shipping / fully-charged / maintenance / charge-worker |
| `1` | **Battery ON** | `battery_cmd1_on` (`0x1335d0`) | `PowerService_TurnOn`, fault recovery |
| `5` | **IdentifyCharger** | `charger_cmd5_identify` (`0x133660`) | `power_control.cpp:0xd4`; first runs the charger clear-test |
| `6` | **Battery RESET** | `battery_cmd6_reset` (`0x133ac0`) | `power_control.cpp:0xf0` *"Sending battery reset command"* |
| `8` | **Shipping mode** | `battery_cmd8_shipping` (`0x133780`) | final step after power-off (*"Entering shipping mode"*) |
| `9` | **Clear fault flags** | `battery_cmd9_clear_fault` (`0x1337d0`) | *"Try to clear battery flags."* |

(`get_xrefs_to battery_send_command` (`0x133590`) returns exactly these six
sites; the `#5`/`#6` opcodes are `strb`-verified in machine code.)

Separate **raw charger clear-test** (`power_control_clear_charger_test_burnin`
`0x133620`, `power_control.cpp:0xca`) — two `system()` `cansend` calls to the
**charger** node `0xA7`: `cansend vcan0 14E23214#A55A00` (set) /
`14E23210#A55A00` (clear), payload `A5 5A 00`.

### BMS reset flow

- **MQTT-triggered:** `maintenance/battery/primary/reset` → `on_mqtt_battery_reset`
  (`0x112a50`) (*"MQTT Request to Reset Primary Battery."*) → cmd `6` → `nanosleep` ~16 s →
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
| `IsPrimaryInserted` | power_control `+0x49` (`get_is_primary_inserted` `0x133450`) | power-control status `frame[2] & 1` |
| `BatteryINS-DET` | monitor `+0x8f` (`get_battery_ins_det` `0x128170`) | battery status `frame[6]>>5 & 1` |
| `BatteryINS-DET FAIL` | monitor `+0x90` (`get_battery_ins_det_fail` `0x128190`) | battery status `frame[6]>>4 & 1` |

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

## E-shifter calibration FSM (`eshifter_calibration.cpp`)

The power service owns e-shifter auto-calibration because it may only run **while
charging** (spinning the hub through its gears needs mains power):
`battery_power_on_sequence` calls `eshifter_request_calibration` on entry to
CHARGING. Reconstructed in
[`src/eshifter_calibration.c`](src/eshifter_calibration.c) (6 functions):

| Addr | Name | Role |
| --- | --- | --- |
| `0x120030` | `eshifter_calibration_ctor` | subscribes `eshifter/state` → FSM and `eshifter/last_calibrated` → restore |
| `0x120a40` | `eshifter_on_last_calibrated` | loads the retained "last calibrated" day-stamp |
| `0x120680` | `eshifter_request_calibration` | rate-limiter: only if **>30 days** since last and state idle (*"Eshifter was last calibrated %d days ago, allowing calibration"*) |
| `0x120ac0` | `eshifter_on_state_update` | the **FSM**, driven by `current_gear` |
| `0x11fee0` | `eshifter_calibration_timeout` | watchdog: retry ≤4×, then block till next charge |
| `0x120310` | `eshifter_calibration_dtor` | unsubscribe + STL teardown (prose) |

**FSM** (`eshifter/state.current_gear` drives it): `IDLE` →(request)→ **REQUEST**:
publish `eshifter/gear/set` (*"Sending eshifter calibration request"*) → **SENT**;
gear hits 0 → **IN_PROGRESS**; gear returns to 1 → *"Eshifter calibration has
completed, eshifter is back to 1-st gear"*, publish retained `eshifter/last_calibrated`
= today → `IDLE`. The **watchdog** (`eshifter_calibration_timeout`) fires if no
state update arrives (*"…eshifter might be disabled"*): <4 attempts → *"Resetting
calibration state to idle…"* (retry); ≥4 → *"Preventing further calibration
attempts until next charging"* + a config override. *(Both
`eshifter_calibration_timeout` and the 6th function were uncarved — recovered via
the string xref; see the sweep below.)*

## vtable completeness sweep

To confirm nothing else was missed the way the battery cluster was, every
`.text` pointer in **`.data.rel.ro`** (`0x184318–0x1853b7`, where the C++ vtables
live) was extracted and diffed against the defined-function list. Result: **172**
distinct vtable→code pointers, **126** of which Ghidra had not carved — but on
inspection they are *all* non-substantive:

- **adjustor thunks** (8 bytes, `sub x0,#imm ; b body`) — C++ `this`-pointer
  adjustment for secondary bases (vendor/compiler);
- **deleting destructors** (`~Class()`: set vtable → dtor members → `operator
  delete`) — vendor-pattern cleanup;
- **default-virtual stubs** (`return;`, e.g. the 7 `IStateTransitions` defaults
  at `0x11c410…470` that PowerService overrides);
- **vendor library class vtables** (mosquitto C++ wrapper, STL iostream /
  filesystem, nlohmann-json, the `vm` library at `0x136xxx+`).

**No substantive missed VanMoof application logic remains** — the battery
power-on cluster was the only one of its kind. (The one further VanMoof function
found, `eshifter_calibration_timeout`, was reached by a *direct* call, not the
vtable, so the string-xref audit caught it, not this sweep.)

**Naming pass (cross-ref of `common_logf` callers).** Every VanMoof function that
logs embeds its `devices/main/<svc>/src/*.cpp` path via `common_logf`, so the
caller set of `common_logf` (`0x158f60`) is the inventory of VanMoof logic. All
of them are now **named** in Ghidra — the last batch renamed ~25 helpers that
were still `FUN_*` (the `rtc_handler`, `wake_on_motion_handler`, the Monitor
charger/OTA-status handlers, `powerservice_enter_shipping`, `switch_control_ctor`,
`bq25672_read_register`, `low_power_system_poweroff`, …). The only `common_logf`
callers left as `FUN_*` are **vendor** (`vm` library `0x1576d0`, mosquitto
wrapper `0x14a4e0`).

## Crash handling & service bootstrap (`service_env.cpp`)

Every main-module C++ service shares `devices/main/common`'s `ServiceEnv`
bootstrap (`service_env_ctor` `0x1409f0`): it builds the `vm` CAN object
(`vm_init`) and the MQTT client, logs *"Starting service version %s on CAN: %s"*,
opens SocketCAN (`vm_can_open`), and installs **`service_env_signal_handler`**
(`0x140370`) for `SIGINT(2)`, `SIGQUIT(3)`, `SIGTERM(15)` and `SIGSEGV(11)`
(table @`0x160d40`).

On **`SIGSEGV`** the handler writes a crash dump:
`/run/media/mmcblk2p6/SEGFAULT_<MM-DD-YYYY_HH-MM-SS>` (created `O_WRONLY|O_CREAT`),
fills it with `backtrace()` → `backtrace_symbols_fd()`, then `exit(1)`. The other
signals just set the shutdown flag and return for a graceful exit. **These
`SEGFAULT_*` dumps persist on the eMMC** (`mmcblk2p6`) — a ready-made forensic
trail of any service that has crashed.

## Attack surface (notes)

Power is a small daemon but it reaches outside its process in a few ways worth
recording (the broker side is in [`../docs/mqtt-bus.md`](../docs/mqtt-bus.md)):

- **Shell-outs (`system()`):** the charger clear/burn-in test
  `system("cansend vcan0 14E232{10,14}#A55A00")` (`power_control.cpp:0xca`) and
  the standby RTC refresh `system("touch …/timesync/clock ; sync")`. Both build
  fixed command strings (no external input spliced in), so no injection here —
  but they are the daemon's exec surface.
- **Privileged effects from the bus:** `power/state/set` drives the 8-state
  machine, `maintenance/battery/primary/reset` issues a CAN battery reset, and
  the state path can `/sbin/poweroff` and write `/sys/power` (suspend). The
  loopback MQTT broker has **no auth** (anonymous + ACL-by-claimed-username), so
  any on-box process that reaches `127.0.0.1:1883` can trigger these — the only
  boundary is the loopback bind and the BLE/modem bridges.
- **Persistent files:** reads `/run/media/mmcblk2p6/bike_id`; writes
  `SEGFAULT_*` crash dumps (above) to the same eMMC partition.

## Next (when continuing)

- [x] `StateManager` gate + dispatch + state enum — done.
- [x] The **`IStateTransitions` per-state handlers** (7 states) → per-state actions — done (above).
- [x] `lipo_control` buck/charge logic (BQ25672) + `device/charger/*` reaction — done (above).
- [x] The 50 ms / 4000 ms timer handlers — done (above).
- [x] The **`vm` CAN wire protocol** + per-signal numeric CAN IDs — done & verified → [`../docs/can-bus.md`](../docs/can-bus.md).
- [x] The 4000 ms charge-supervisor worker (sub-state machine) — done (above).
- [x] BMS type detection, reset + command set, insertion detect — done (above).
- [x] **Battery-string completeness audit** — every battery/charge log string is
      now accounted for. Surfaced + carved the **power-on/buck supervisor virtuals**
      Ghidra missed (`battery_power_on.c`, above); no battery strings remain
      unreferenced.
- [x] **E-shifter calibration FSM** reconstructed (`eshifter_calibration.c`),
      incl. the two uncarved functions Ghidra missed.
- [x] **vtable completeness sweep** — diffed all `.data.rel.ro` code pointers vs
      the function list; no further substantive missed VanMoof functions (above).
- [x] **Crash handler + service bootstrap + attack-surface** documented (above).
- [x] **Naming pass** — every VanMoof `common_logf` caller is now named in Ghidra
      (~25 final helpers incl. `rtc_handler` + `wake_on_motion_handler`); only
      vendor `vm`/mosquitto loggers remain `FUN_*`.
- [ ] Confirm the **supplier matcher** (Panasonic vs DynaPack) in the `update`
      binary (already in Ghidra) — it's not in `power`.
- [ ] Cross-reference the CAN node map against the other ECU targets (see
      `can-bus.md` §6).

## Ghidra

Renamed functions / analysis state for this target are tracked here; the running
function map is in [`ghidra/exports/`](ghidra/exports/) (kept in sync as
functions get named, per the repo convention).
