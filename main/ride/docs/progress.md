# ride — reconstruction progress

> Per-function tracker for the i.MX8 **ride** service (pedal-assist controller).
> Stripped AArch64 C++ ELF, base `0x100000`, 465 fns; Ghidra `/S5-v1.5/OS/ride`.
> Architecture: [`../../docs/ride.md`](../../docs/ride.md).

## Status: RECONSTRUCTED to C — 8 TUs compile clean (`make`, -Wall -Wextra -Wpedantic); 112 functions named in Ghidra

Behaviour-oriented C reconstruction in `src/` (`ssp_protocol`, `serial_port`,
`motor`, `ride_service`, `main`, `power`, `motor_sensor_od`,
`speed_ratio_ride_strategy`) + per-module `include/*.h` + canonical
`include/ride_common.h` + `Makefile` — same convention as the other i.MX8
targets. The SSP protocol (SLIP framing + CRC-16/MODBUS), the assist filter/EMA,
the power SOC-derate and the assist-table lookup are reconstructed faithfully;
the C++ runtime, the CANopen-OD framework and the IMotor/IPower/IPedalSensor/
RideStrategy interfaces are modelled opaque. (Decode mapping below unchanged.)

The RideService assist gate, the full SSP motor protocol, the telemetry/filter
path, the CANopen sensors and the power gates are identified, named, documented.
The ELF was undisassembled on import (gap pattern); app functions were
`create_function`'d before decompile. Export: `ghidra/exports/ride_program.json`.

## Verification vs OEM (full pass — 5-agent workflow)

Every reconstructed function decompiled + compared to the OEM. First pass:
**44 behaviour-eq, 8 divergent**. All 8 were real bugs, now fixed:

| # | Function | Bug → fix |
|---|---|---|
| 1 | `ride_main` | `ride_service_ctor` call mis-slotted args — dropped the SspProtocol; OEM arg4=ssp, arg5=strategy |
| 2 | `ride_service_publish_telemetry` | telemetry field 1 read `ipower_voltage(power)` (twice); OEM is `imotor_wheel_speed(motor)` |
| 3 | `motor_update_brake_lights` | OD key `ke_level` → `ride/brake_level`; accel log `%f` → `%.3f m/s²` |
| 4 | `motor_handle_request` | motor-type names `"S"`/`"X"` → `"A5"`/`"S5"` |
| 5 | `motor_log_status_bits` | first status-bit entry `{0x150,1}` → `{0x151,0}` (word bit 8) |
| 6 | `power_compute_max_discharge_current` | derate log fired unconditionally; OEM logs only on the `target<known` branch |
| 7 | `serial_port_configure` | wrong `c_cflag` mask + missing `c_oflag=0`/`c_lflag` clears |
| 8 | `serial_port_open` | exclusive-lock path: double `ioctl(TIOCEXCL)` + wrong string → single ioctl + `"Error, device is locked"` |

After the fixes the build is clean (8 TUs, -Wall -Wextra -Wpedantic). Verified
verbatim across the rest: the SSP CRC-16/MODBUS + SLIP framing + frame layout +
type dispatch, the assist-filter float coefficients (PI, cadence/30, cos/20), the
current EMA (0.98/0.02 ↔ 0.5/0.5), the SOC bands + SufficientSOC≥13, the
`!!EnableMotor` gate, and the strategy region dispatch + opcodes.

## Core / spine + RideService (`main.cpp`, `ride_service.cpp`)
- `common_logf` `0x147630`; `ride_main` `0x10a1f0` (opts `-i -p -s -t`).
- `ride_service_ctor` `0x1106e0` (IMotor/IPower/IPedalSensor/RideStrategy; size 0x180).
- **`ride_service_should_enable_motor` `0x11bb00`** — `EnableMotor = PoweredOn && Pedalling && SufficientSOC`; the `!!EnableMotor: …` log line.
- `ride_service_publish_telemetry` `0x1129f0` (SSP `0x1e` + filters); `ride_app_ctor` `0x11b200`, `ride_app_run` `0x1200e0`.

## SSP protocol (`ssp_protocol.cpp`, `common/serial_port.cpp`)
SLIP (`0xC0`/`0xDB`) + CRC-16/MODBUS (`ssp_crc16` `0x12a340`). Frame `[0x14,type,seq,opcode,payload,CRC]`; types `0x05` ack / `0x06` cmd / `0x07` report.
- `ssp_protocol_ctor` `0x122630`, `_start` `0x1245a0`, `_stop` `0x124700`, `_dtor` `0x124780`.
- `ssp_receive_loop` `0x122d50`, `ssp_send_loop` `0x124100`, `ssp_enqueue_frame` `0x122bf0`, `ssp_send_ack` `0x122270`, `ssp_frame_begin` `0x1224c0`, `ssp_frame_append` `0x122460`, `ssp_write_escaped_byte` `0x124090`.
- transport: `serial_port_open` `0x12cf90`, `_configure` `0x12ce60`, `_read_impl` `0x12d4e0`, `_close` `0x12d2f0`, `serial_transport_{write,read}_byte` `0x12c6d0`/`0x12d6d0`.

## Motor (`motor.cpp`)
- `motor_handle_request` `0x10ff20`, `motor_publish_value` `0x10fdb0` (op 0x19 = v/100), `motor_request_version` `0x110880` (op 0x0a), `motor_get_version` `0x110940`.
- `motor_assist_filter` `0x110bd0` (biquad), `motor_current_smooth` `0x110cc0` (asym EMA), `motor_log_status` `0x110d90`, `motor_update_brake_lights` `0x111b10` (ke_level).

## Sensors + power (`motor_sensor_od.cpp`, `power.cpp`)
- `motor_sensor_od_model_callback` `0x113c70`, `_handle_report` `0x113da0`, getters `0x113b20…`.
- `power_ctor` `0x119060` (OD: temperature/state/capacity), `power_compute_max_discharge_current` `0x118d00` (SufficientSOC when SOC≥13), `power_is_{powered_on,charging,sufficient_soc}` `0x118c70/60/80`, OD callbacks `0x119980/9d0/a30`.

## Strategy (`speed_ratio_ride_strategy.cpp`)
- `speed_ratio_*` (ctor `0x116450`); `ComputeCommand` → 14-byte cmd (op `0x1c`/`0x1b`/`0x17`).
- **Assist curve is DATA-DRIVEN** — four 14-byte/entry region tables via `.bss` ptrs (`0x175510/28/40/58`), levels 0–4 + boost. Numeric values are in runtime `.bss`, NOT the static image.

## Open / follow-ups
- Recover the assist-curve table constants (runtime `.bss` dump or the writer).
- `SpeedRideStrategy` (`-t 0` speed-control) not decoded.
- After this: recheck the `power_pedal` torque/rpm OD indices against ride's sensor reads (per TODO).
