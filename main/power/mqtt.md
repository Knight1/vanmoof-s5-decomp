# `power` service — complete MQTT catalog

Every MQTT topic the `power` service subscribes to or publishes, with the
handler/publisher function (Ghidra VA, image base `0x100000`) and payload. The
subscriptions are registered in `PowerService_ctor` (`0x116350`), `Monitor_ctor`
(`0x129520`) and the `PowerControl`/`Eshifter` ctors; publishes come from the
decoders/handlers. Broker = the loopback bus (user `power-service`); the
`power_control_*` / `battery_primary_*` / `charger_*` names are **OD/CAN**
signals carried over the same `vm` layer (see [`../docs/can-bus.md`](../docs/can-bus.md)).

## Subscribe (19)

| Topic | Handler | Payload |
| --- | --- | --- |
| `power/state/set` | `on_mqtt_state_set` `0x11c3a0` | int `power_state` → `StateManager_OnStateRequest(state, force=0)` |
| `power/state/extend_timeout` | `on_mqtt_state_extend_timeout` `0x11c320` | int seconds → StateManager set-extend-timeout |
| `power/low_power_extend` | `on_mqtt_low_power_extend` `0x116280` | int seconds → extend standby timer by N·1000 ms |
| `maintenance/battery/primary/reset` | `on_mqtt_battery_reset` `0x112a50` | (no body) → battery CAN **cmd 6** reset → 16 s → STANDBY |
| `modem/system/time` | `on_mqtt_modem_time` `0x115ad0` | `{"ret":1,"time":<epoch>}` → set RTC / clock file when in standby |
| `device/charger/voltage` | Monitor (`0x129520` / `0x1264e0`) | charger output voltage |
| `device/charger/current` | `0x12d950` | charger output current |
| `device/charger/mode` | `0x12d790` | string `Success`/`FailRetry`/`InProgress`… → charger bg-update flag |
| `device/charger/finished` | Monitor (`0x129520`) | charger finished |
| `power_control_state` | `power_control_on_state` `0x133820` | 8-byte status frame: `[0]`=state (0 off/1 on/3 standby/5 identify), `[2]&1`=IsPrimaryInserted, `[2]>>2&1`=INS-DET FAIL |
| `power_control_control` | identity mgr `0x125820` | control signal |
| `power_control_measurements` | identity mgr `0x135830` | measurements |
| `power_pedal_power_switch_control_init` | identity mgr `0x125820` | init |
| `battery_primary_battery_state_init` | identity mgr `0x125820` | init handshake (charger node `0xA7`) |
| `eshifter/state` | `Eshifter_onStateUpdate` `0x120ac0` | `{current_gear}` → drives the calibration FSM |
| `eshifter/last_calibrated` | `0x120a40` | last calibrated gear (stored) |
| `update/progress` | Monitor `0x12d3d0` | update progress |
| `update/background_update/progress_info/charger` | `0x12d3d0` | `{device,…}` → clears charger bg-update flag |
| `update/stage2/device_update_started` | Monitor `0x12d950` | update started |

## Publish (42)

### Power-state machine
| Topic | Publisher | Payload |
| --- | --- | --- |
| `power/state` | `StateManager_ChangeState` `0x11acb0` | int `power_state`, **qos 5, retained** (re-emitted by OnStateRequest) |
| `power/state/status` | `ChangeState` (0) / `OnStateRequest` | int code: 0 / 1=RETRY / 2=IS_ACTIVE / 3=DENIED, qos 5, not retained |
| `power/low_power` | `0x124860` (1) / `0x125030` (2) | int low-power phase (1 or 2) |
| `power/deep_sleep` | `0x124c40` | bool deep-sleep entry |

### Primary (Panasonic) battery — decoded from CAN (node `0xA4`)
| Topic | Publisher | Payload |
| --- | --- | --- |
| `power/battery/primary/info/voltage` | `battery_voltage_decode` `0x1285c0` | u16 mV |
| `power/battery/primary/info/charge_voltage` | `battery_charging_decode` `0x128730` | u16 mV |
| `power/battery/primary/info/charge_current` | `battery_charging_decode` `0x128730` | u16 mA |
| `power/battery/primary/info/health` | `battery_health_decode` `0x1289c0` | health % |
| `power/battery/primary/info/cycles` | `battery_health_decode` `0x1289c0` | cycle count |
| `power/battery/primary/info/soc` | `battery_capacity_decode` `0x12d150` | raw RSOC % |
| `power/battery/primary/info/soc_app` | `battery_capacity_decode` → `soc_to_soc_app` `0x12cf80` | display SoC % (piecewise-linear remap) |
| `power/battery/primary/info/temperature` | `battery_temperature_decode` `0x12da90` | JSON: 4 sensors {cell 1, cell 2, chg mos, dsg mos} |
| `power/battery/primary/info/discharge_current` | `battery_temperature_decode` `0x12da90` | discharge current mA |
| `power/battery/primary/info/max_current` | `battery_temperature_decode` `0x12da90` | max current mA |
| `power/battery/primary/info/power` | `battery_temperature_decode` `0x12da90` | double W = `(I/1000)·(V/1000)` |

### Internal LiPo battery — read from the TI bq27542 gauge sysfs (`lipo_publish_info` `0x12ba60`)
| Topic | sysfs source (`/sys/class/power_supply/bq27542-0/…`) |
| --- | --- |
| `power/battery/lipo/info/soc` | `charge_now` |
| `power/battery/lipo/info/capacity` | `capacity` |
| `power/battery/lipo/info/capacity_lvl` | `capacity_level` (string) |
| `power/battery/lipo/info/voltage` | `voltage_now` |
| `power/battery/lipo/info/current_now` | `current_now` |
| `power/battery/lipo/info/charge_full` | `charge_full` |
| `power/battery/lipo/info/charge_full_design` | `charge_full_design` |
| `power/battery/lipo/info/cycles` | `cycle_count` |
| `power/battery/lipo/info/health` | `health` (string) |
| `power/battery/lipo/info/status` | `status` (string) |
| `power/battery/lipo/info/temp` | `…/hwmon1/temp1_input` |
| `power/battery/lipo/info/pwr_avg` | `power_avg` |

### Other
| Topic | Publisher | Payload |
| --- | --- | --- |
| `IsPrimaryInserted` | `power_control_on_state` `0x133820` (od-override) | bool `(frame[2]&1)^1`; warns *"Primary battery not detected"* |
| `battery_primary_battery_status` | `battery_status_decode` `0x1268d0` | ~40 per-bit booleans + INS-DET fields |
| `battery_primary_battery_warning` | `battery_warning_decode` `0x1275a0` | ~20 alarm booleans |
| `device/charger/connected` | `Monitor_ctor` `0x129520` | bool |
| `modem/vars/update` | `timer_50ms_poll` `0x1146d0` | register/var snapshot |
| `eshifter/gear/set` | `Eshifter_onStateUpdate` `0x120ac0` | calibration request (gear `'t'`=0x74) |
| `eshifter/last_calibrated` | `0x120ac0` | written back on calibration complete |
| *(raw)* `cansend vcan0 14E232{14,10}#A55A00` | `power_control_clear_charger_test_burnin` `0x133620` | charger node `0xA7`, clear test/burn-in, magic `A5 5A 00` |
| *(OD)* power-control cmd `0x14603040` (node `0xA3`) | `battery_stamp_command` `0x138f90` / `battery_send_command` `0x133590` | `frame[0]`=opcode (0 off/1 on/5 identify/6 reset/8 shipping/9 clear-fault) |

> The `device/charger/{voltage,current}` numeric publishers
> (`monitor_charger_connected_publish` `0x128cc4`) live in `monitor.cpp`. The
> `power_control_*` / `battery_primary_*` / `charger_*` names are CAN OD signals,
> not loopback-MQTT topics. System-wide bus + ACL: [`../docs/mqtt-bus.md`](../docs/mqtt-bus.md).
