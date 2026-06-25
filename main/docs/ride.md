# ride service (pedal-assist controller)

`/usr/bin/ride` is the i.MX8's **pedal-assist controller** — it reads the pedal
torque/cadence + battery state, decides how much motor assist to apply, and
drives the motor controller over a serial link. Stripped AArch64 C++ ELF (base
`0x100000`, 465 functions, Ghidra `/S5-v1.5/OS/ride`), built from
`devices/main/ride/src/`. Modules: `main`, `ride_service`, `motor`,
`motor_sensor_od`, `power`, `speed_ratio_ride_strategy`, `ssp_protocol`.

> The ELF imports almost entirely undisassembled (only `_INIT`/PLT defined); the
> app code lives in indirect-dispatch gaps and was recovered with
> `create_function` ([[subecu-app-in-undisassembled-gap]]).

Logger: `common_logf` (`0x147630`, `(file,line,level{1=DBG/2=INFO/3=WARN/4=ERR},
fmt,…)`). `ride_main` (`0x10a1f0`) parses `-i`, `-p <port>` (default
`/dev/ttymxc3`), `-s <kmh>` (simulate — spins the motor), `-t <type>`
(0 = speed control, 1 = speed-ratio, default 0), builds the object graph
(SerialTransport → SspProtocol → Power → Strategy → RideService → App) and runs.

## RideService — the assist core

`RideService(IMotor, IPower, IPedalSensor, RideStrategy)` (ctor `0x1106e0`, size
`0x180`, multiple-inheritance: it *is* the `ISspHandler` for motor reports). The
assist decision is **`ride_service_should_enable_motor`** (`0x11bb00`,
`ride_service.cpp:0x115`):

```
EnableMotor = (PoweredOn && Pedalling && SufficientSOC) ? <assist from IPedal vt+0x20> : 0
```

It logs the famous rate-limited line verbatim:
`!!EnableMotor: %d, Pedalling: %d, PoweredOn: %d, Spinning: %d, SufficientSOC: %d, Motor comm running: %d`.
The five gates are vtable calls on the sub-interfaces: `PoweredOn`=IPower vt+0x28,
`Pedalling`=IPedalSensor vt+0x10, `SufficientSOC`=IPower vt+0x28,
`Spinning`/assist=IPedal vt+0x20, `MotorCommRunning`=IMotor vt+0x30.

### Telemetry + filtering
`ride_service_publish_telemetry` (`0x1129f0`) reads the pedal sensor (speed/torque)
and motor (current/speed), runs:
- **`motor_assist_filter`** (`0x110bd0`) — a 2nd-order/biquad pedal-assist filter
  (cadence/30 min 1.5, cosine term /20, state in `.bss`).
- **`motor_current_smooth`** (`0x110cc0`) — an asymmetric EMA (0.98/0.02 rising,
  0.5/0.5 falling).

…then emits SSP data-report opcode `0x1e` `{wheel_speed, pedal_speed*100, torque,
motor_current, motor_speed*10}` plus periodic `0x0c`/`0x0e`. `motor_log_status`
(`0x110d90`) logs `wh_speed:%d pdl_speed:%d pdl_cnt:%d pdl_torque:%d mtr_tmp:%d
drv_tmp:%d current:%d` + the gate-driver status-register bit decode (rate-limited
mod-20). `motor_update_brake_lights` (`0x111b10`) keeps a 10-sample speed window,
derives a brake level (2 = hard decel < −5), and issues a `ke_level` LED command.

## SSP protocol (the motor serial link)

`ssp_protocol.cpp` — a **SLIP-framed, CRC-16/MODBUS** serial protocol to the motor
controller over `/dev/ttymxc3` (`SspProtocol` ctor `0x122630`, size `0x1c0`).

- **SLIP framing:** delimiter `0xC0` (start+end), escape `0xDB`
  (`0xDB,0xDC`→`0xC0`, `0xDB,0xDD`→`0xDB`). TX escaping `ssp_write_escaped_byte`
  (`0x124090`).
- **CRC:** `ssp_crc16` (`0x12a340`) — CRC-16/MODBUS (reflected poly `0xA001`,
  seed `0xFFFF`), appended low-then-high (also escaped); RX validates
  `crc16(whole de-framed buffer) == 0`.
- **Frame** (after de-framing): `[0]=0x14 marker, [1]=type, [2]=seq, [3]=opcode,
  payload…, CRC16`. Types: **`0x05`** ack (seq-matched in a pending RB-tree),
  **`0x06`** command (→ handler `vt+0x10` then `ssp_send_ack`), **`0x07`** report
  (value = `buf[6]*0x100+buf[5]`, opcode `buf[3]` → handler `vt+0x18`).
- **Threads:** `ssp_protocol_start` (`0x1245a0`) spawns `ssp_receive_loop`
  (`0x122d50`) + `ssp_send_loop` (`0x124100`). The send loop auto-assigns the
  sequence into `buf[2]` and writes `len-7` at `buf+5` for payloads ≥ 8.
- **Send path:** build with `ssp_frame_begin`(`0x1224c0`)/`ssp_frame_append`
  (`0x122460`), enqueue via `ssp_enqueue_frame` (`0x122bf0`).
- **Transport:** `common/src/serial_port.cpp` (`SerialPort` over POSIX termios —
  exclusive open `TIOCEXCL`, raw mode, `select()`-with-timeout read).

### Motor opcodes
`0x0a` request version · `0x19` set/report motor speed (= value/100) · `0x1a`
motor-type · `0x1e` telemetry data-report · `0x0c`/`0x0e` periodic.

## Sensors + power

- **`motor_sensor_od.cpp`** — CANopen-OD reader: a bike-model select callback
  (A5/S5 → motor-config struct) and a report parser decoding i16 motor speed,
  pedal value, and status bits.
- **`power.cpp`** — the `IPower` impl (size `0x80`). Registers CANopen OD entries
  `battery_primary_battery_temperature`, `power_control_state`,
  `battery_primary_battery_capacity` (SOC). `power_compute_max_discharge_current`
  (`0x118d00`) derates a max-discharge current from SOC + temp-derated capacity
  (0.85/0.70 band factors) and sets **SufficientSOC when SOC ≥ 13** (`0x0d`).
  `IsPoweredOn`/`IsCharging` derive from the `power_control_state` enum.

## The assist strategy (data-driven)

`SpeedRatioRideStrategy` (`speed_ratio_ride_strategy.cpp`, ctor `0x116450`):
`ComputeCommand` returns a 14-byte command (byte[0] = type → SSP opcode
`0x1c`/`0x1b`/`0x17`, then up to six u16 params). **The assist curve is
data-driven, not a formula** — four 14-byte-per-entry tables (assist levels 0–4 at
offsets `0/0xe/0x1c/0x2a/0x38` + a boost entry at `0x46`), one per region,
addressed through `.bss` pointers (`0x175510/28/40/58`). **The numeric curve
values are NOT in the static image** — the tables live in uninitialized `.bss`
filled at runtime (region/config load elsewhere); recovering them needs a runtime
`.bss` dump or the writer. The sibling `SpeedRideStrategy` (`-t 0`) wasn't decoded.

## MQTT topics

| Direction | Topics |
| --- | --- |
| publishes | `ride/info/{speed,torque,pedal_rpm,motor_speed,motor_current,motor_temperature,distance,calories}`, `ride/boost` |
| subscribes | `power/state{,/status,/extend_timeout}`, `settings/assist_level`, `settings/region`, `ride/boost` |

## Status

112 functions named in Ghidra (`main/ride/ghidra/exports/ride_program.json`).
The RideService assist gate, the full SSP motor protocol, the telemetry/filter
path, the CANopen sensors and the power gates are mapped. Open: the data-driven
assist curve constants (runtime `.bss`), and `SpeedRideStrategy` (`-t 0`).
