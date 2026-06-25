# ux service (UX orchestrator)

`/usr/bin/ux` is the i.MX8's **user-experience orchestrator** — the bike's
behavioural brain. It is a stripped AArch64 C++ ELF (base `0x100000`, 602
functions, imported in Ghidra at `/S5-v1.5/OS/ux`) built from
`devices/main/ux/src/`. It ties together the lock, lights, sound, sensors, ride
telemetry, alarm/theft response, Apple Find-My, the power button and the power
state machine — everything over the MQTT message bus, coordinated by a
**6-state strategy machine**.

Every app function logs through `common_logf` (`0x1a9590`,
`(file, line, level{1=DBG,2=INFO,3=WARN,4=ERR}, fmt, …)`); the first arg is the
module's `devices/main/ux/src/<module>.cpp` path, which is how functions map to
modules in the stripped image.

## Central object — `UXService`

`UXService` (ctor `ux_service_ctor` `0x12e3e0`; vtable `0x1fae00`; typeinfo
`0x1fad88`; mangled `9UXService`) owns every subsystem client and **derives from
`common::StateClient`** (so `UXService::run` == `StateClient::start`,
`ux_service_run` `0x1854b0`). `main` (`0x10ede0`, carved out of the `_INIT_1`
static-init blob) parses `-p/--path`, builds the `ux-service` app framework,
pulls 3 MQTT/bus handles from it, then constructs + runs + destroys the
`UXService`.

Construction (`0x12e3e0`): installs the vtable at object `+0x60`, embeds a
`StorageManager` at `+0x60`, nulls the active strategy (`+0x1088`) and state enum
(`+0x1090`), builds a power sub-client, and subscribes `ux/power`
(`ux_service_on_power_msg` `0x12e2c0`) + `power/deep_sleep`.

## The 6-state strategy machine

State is **polymorphic dispatch**, not a switch. The 7 `UXService::To<State>`
virtuals each run the same hot-swap: log `To<State>` → destroy the current
`IStateStrategy*` at `+0x1088` → `operator new` the concrete strategy → call its
ctor (which *is* the on-enter hook) → store it → write the state enum at
`+0x1090` → tail-call `ux_service_on_state_entered` (`0x13ef20`, which persists
the new state via `StorageManager`). A running strategy (or an MQTT/CAN handler)
requests a transition by calling the appropriate `To<State>` virtual.

| Enum | State | `To<State>` | strategy ctor | obj size | role |
| --- | --- | --- | --- | --- | --- |
| 1 | Shipping | `0x12dbb0` | `0x134d60` | 0x10 | sealed-for-shipping (minimal) |
| 2 | Standby | `0x12dc70` | `0x133d50` | 0x5a0 | bike off/idle; power-button arm, 15 s sleep timer |
| 3 | Operational | `0x12dd30` | `0x131900` | 0x440 | riding/unlocked; ride+light+sound, idle anim |
| 4 | Charging | `0x12ddf0` | `0x12ef30` | 0xe8 | SOC LED ring, charge start/stop UI |
| 5 | Updating | `0x12deb0` | `0x134ea0` | 0x18 | OTA: `firmware_update_loop`, progress anim |
| 6 | Alarm | `0x12df70` | `0x135e60` | 0x5c0 | theft response (3-stage escalation) |
| 7 | Maintenance | `0x12e030` | `0x136a10` | 0x98 | battery-locked; `sec_battery_*` sounds |

Strategy ctors reach the `UXService` context (`+0x60`) via accessor thunks
(`FUN_0013db*`): `db80`=sound/LED, `db60/db90`=lock, `dbc0`=MQTT publisher,
`db70`=ride/motor, `dbd0`=light, `dbe0`=bike-state, `dbf0`=BLE, `dba0`=sensors.

### Notable strategy behaviour
- **Operational** (`0x131900`): ~13 bike-event handlers — lock/unlock, motor
  assist on/off (`ride_publish_boost`), animation theme, shutdown — plus a 1000 ms
  idle-animation timer (LED ring anim `0x545`) and an 8000 ms idle timeout.
- **Charging** (`0x12ef30`): 3000 ms SOC-animation tick; publishes `plugged`,
  plays charge-start sound `0x696`; if not actually charging, requests power-off
  back to standby and publishes charge `success`/`failed`.
- **Standby** (`0x133d50`): arms the power button (theft → stay locked; else set
  state + 3000 ms long-press → power-on); 15000 ms idle/sleep timer; shutdown
  path plays the shutdown sound + tears down find-my.
- **Alarm** (`0x135e60`): theft response. Reads the alarm level from the elock OD
  (`0xFF`=Theft255 / `0x03`=Alarm3 / `0x02`=Alarm2). Alarm2 flashes lights
  (pattern `0x249`) + loops `alarm_2_urgent_loop`, arming a **120 s** escalation
  timer to Alarm3; Alarm3 mutes sound + stops find-my/tracking publishing. The
  lock-changed / button callbacks de-escalate.
- **Updating** (`0x134ea0`): `firmware_update_loop` sound, light mode 3, uploads a
  progress animation, loops light pattern `0xb4`.
- **Maintenance** (`0x136a10`): plays `sec_battery_locked`; a button press plays
  `sec_battery_unlock` and requests `ToStandby`.

## Subsystems

- **Lock** (`elock.cpp` + `backup_unlock.cpp`) — `ElockHandler` owns the software
  lock state (`+0xb2`: 1=locked, 3=unlocking). `elock_init` (`0x164bd0`) registers
  motor VM-call handlers, loads persisted state (default unlocked), and arms a
  **5000 ms auto-relock** timer (`elock_software_lock` `0x163340`).
  `elock_request_unlock` (`0x1602f0`) issues the motor unlock VM call, persists
  `unlocking`, publishes `ux/lock/info/state`. `elock_on_state_report`
  (`0x162d10`) reconciles the motor's physical state and logs *"Elock has been
  forced unlocked"* on an unexpected open. **`backup_unlock`** is the manual
  fallback code: `backup_unlock_check_code` (`0x170e00`) does a **plaintext**
  byte-compare of a 3-symbol code (**no crypto**), with a 3-strike lockout —
  consistent with [[elock-aes-not-used-for-unlock]].
- **Security** (`alarm.cpp`, `findmy.cpp`, `reset.cpp`) — `alarm_set_state`
  (`0x145dd0`) holds the alarm level and publishes `ux/alarm/state`. `findmy` is
  VanMoof glue around the Apple FMNA stack (which lives in the BLE service, out of
  scope): subscribes `ble/findmy/*`, publishes `certified`/`control`/`toggle`.
  `reset` is factory-reset-by-button-hold (`reset_on_press` `0x178c10` /
  `reset_on_release` `0x178d70`; early release cancels).
- **Ride / Light / Power-button** (`ride.cpp`, `light.cpp`, `power_button.cpp`) —
  `ride` subscribes `ride/info/{speed,power,distance}` + `brake_level`, and
  **maintains the persistent trip odometer** (`ride_update_distance` `0x156450`
  validates the motor absolute odometer, accumulates a monotonic total, persists
  via `StorageManager`; `ride_restore_distance` reads it back at boot) — this is
  the i.MX8-side odometer the motor-odometer question pointed at. `light` does
  auto-headlight via a lux ring-buffer with hysteresis (`light_on_light_sample`
  `0x15d820`, beam 200/50). `power_button` reads the SNVS power-key
  (`/dev/input/...snvs-powerkey-event`, `KEY_POWER` `0x74`) on a poll thread, with
  press(1000 ms)/release(400 ms) timers + multi-press counting.
- **Connectivity** (`ble.cpp`, `bike.cpp`, `update_service_client.cpp`) — `ble`
  tracks per-connection auth state (state 3 = authenticated) which gates
  touch-unlock; `update_service_client` bridges `update/ux/finished` + `update/stage`
  to the Updating strategy; `bike` does startup feedback + the pride/fireworks
  animation.
- **StorageManager** (`storage_manager.cpp`) — the persistent key/value settings
  store embedded in `UXService`. `storage_manager_init` (`0x141ea0`) seeds default
  keys (`on_theme`, `g_lights`, `r_volume`, `shipping`=6000, …), loads device
  identity (`info/ecu_serial`, `info/bike_id`, `info/sku`) and re-publishes it,
  and persists into 3 category files. `storage_manager_set` (`0x119da0`) throws
  *"Key not found"* on unknown keys and publishes changes unless category 2.

## MQTT topic map

| Direction | Topics |
| --- | --- |
| ux publishes | `ux/lock/info/state`, `ux/alarm/state`, `ux/sound/play`, `ux/button`, `ux/info/distance`, `info/{ecu_serial,bike_id,sku}`, `ble/findmy/{certified,control}`, `update/finished` |
| ux subscribes | `ux/power`, `ux/lock/{unlock,software_lock,touch_unlock_request,touch_unlock_activate,locking_while_riding}`, `ux/sensor/{air,humidity,light,temperature}`, `ux/tracking/alarm/imu/triggered`, `power/{deep_sleep,state,state/status,low_power_extend}`, `power/battery/primary/info/{power,soc_app}`, `ride/{info/speed,info/power,info/distance,brake_level,boost}`, `ble/connections/{handle/{id},info}`, `ble/findmy/*`, `update/{ux/finished,stage}` |

## Status

150 functions named in Ghidra (`main/ux/ghidra/exports/ux_program.json`). The
state machine, the subsystem clients and the MQTT wiring are mapped. Per-module
detail in [`../ux/docs/progress.md`](../ux/README.md). The bulk of the remaining
unnamed functions are the C++ runtime (STL containers, `std::function`
glue, nlohmann::json) and the `common`/`lib` framework — vendor, not VanMoof app
code.
