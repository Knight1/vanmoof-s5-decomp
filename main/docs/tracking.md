# tracking service (anti-theft / location)

`/usr/bin/tracking` is the i.MX8's **anti-theft / tracking service**. Stripped
AArch64 C++ ELF (base `0x100000`, 424 functions, Ghidra `/S5-v1.5/OS/tracking`),
built from `devices/main/tracking/src/`. It runs a small theft state machine fed
by three trigger sources — **IMU movement**, **inbound SMS**, and **cellular
location polling** — and reports outward over MQTT to `ux` and the modem.

Logger: `common_logf` (`0x135b10`, `(file, line, level{1=DBG/2=INFO/3=WARN/4=ERR},
fmt, …)`); arg0 is the per-module `devices/main/tracking/src/<module>.cpp` path.
`tracking_main` (`0x107c60`, carved from the static-init blob) builds the app
context, constructs the `Movement` (IMU), `CellLocator` (cellular) and the
state-machine `App`, then runs blocking.

## State machine (`tracking_service.cpp`)

The `App` object (ctor `tracking_service_app_ctor` `0x112e10`) derives from
`common::StateClient`. State enum (`tracking_state_to_string` `0x123890`):

| Value | State |
| --- | --- |
| 0 | OFF |
| 1 | AUTO (armed by movement) |
| 2 | THEFT (armed by SMS) |

`tracking_service_set_state(self, state, publish)` (`0x113810`) is the single
mutator: no-op if unchanged, logs *"To tracking state : %s"*, sets the state via
`StateClient`, and (when `publish`) publishes it **retained** to `tracking/state`
(key `ng_state`). State is restored from the retained topic at boot
(`tracking_service_on_tracking_state` `0x114130`).

### Triggers
- **IMU movement** → `tracking_service_on_alarm_state` (`0x113d20`, subscribes
  `ux/alarm/state`): when the value `== 3` and not already THEFT, sets **AUTO**.
  The chain is: `Movement` publishes `ux/tracking/alarm/imu/triggered` → `ux`
  relays it as `ux/alarm/state` → here.
- **SMS** → `tracking_service_on_sms` (`0x114300`, subscribes `modem/sms`):
  `sms_message_parse` (`0x115d50`) reads the JSON `"tracking"` key → `enabled`=0 /
  `disabled`=1 / else `-1`, pushes the code onto a command deque. The worker
  `tracking_service_command_thread` (`0x113e30`) dispatches: **0 (enabled) →
  THEFT**, **1 (disabled) → OFF**, **2 (locate) →** `tracking_service_play_alarm_sound`
  (`0x113350`, publishes `ux/sound/play`).
- **Cellular** → `CellLocator` (ctor `cell_locator_ctor` `0x10af70`, RTTI
  `_ZN11CellLocator`): a poll thread (`cell_locator_poll_thread` `0x10b6d0`)
  requests a cellular fix from the modem at an interval keyed by the tracking
  type (`modem/tracking/type/set`): **2 / 30 / 240 / 5 min**.
  `cell_locator_on_location` (`0x10b410`, subscribes `modem/location/cellular`) is
  a **pure observer** — it logs the fix but computes no estimate and re-publishes
  nothing; the MCC/MNC/LAC/CID payload is built by the modem service.

## Movement / IMU (`movement.cpp`)

`Movement` (ctor `movement_ctor` `0x111df0`) subscribes
`ux/tracking/alarm/imu/triggered`. State (`obj+0xa0`): 0=unset, 1=kMoving,
2=Stationary. An incoming IMU trigger → `movement_set_moving` (`0x1121e0`,
debounced); a **30-minute** (1,800,000 ms) timer with no further trigger reverts
to `movement_set_stationary` (`0x112060`). The **accelerometer threshold/sampling
is not here** — the IMU decision arrives pre-made as the MQTT trigger message;
this module is just the state holder + debounce. (Firmware log string
*"MovementState : Sationary"* [sic] preserved verbatim.)

## MQTT topic map

| Direction | Topics |
| --- | --- |
| publishes | `tracking/state` (retained), `ux/sound/play` (locate), `ux/tracking/alarm/imu/triggered` (`Movement`) |
| subscribes | `tracking/state`, `modem/{info/device, sms, location/cellular, tracking/type/set}`, `ux/alarm/state` |

## Scope / boundaries
- **No outbound SMS formatting** and **no location estimate** are computed in
  this service — both are the modem service's job; `tracking` only *consumes*
  inbound SMS + the cellular-fix reply and *decides* the theft state.
- 36 app functions named (`main/tracking/ghidra/exports/tracking_program.json`).
  The rest of the 424 are nlohmann::json / libstdc++ / `std::function`+`std::deque`
  glue and the shared `common`/`lib` framework — vendor.

## Open question
The SMS command worker handles a **code 2 (locate)** path, but
`sms_message_parse` only ever returns `0/1/-1` in this build — so the locate
command originates somewhere other than this parser (another SMS path or a
different command source). Flagged for a follow-up trace.

See [`../tracking/docs/progress.md`](../tracking/README.md) for the per-function
tracker.
