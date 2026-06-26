# MQTT topic catalog — VanMoof S5/A5 (i.MX8 `main` domain)

Every i.MX8 service talks over a **local MQTT bus** (a `mosquitto` broker on
`localhost`, started by `mosquitto.service`; all the C++ services and the Go
`gateway` connect to it). The bus is the bike's internal message fabric: each
service publishes its state/telemetry and subscribes to the inputs it needs.
The Go **`gateway`** is the only bridge off-bike — it mirrors a curated slice of
the bus to/from **AWS IoT** (Device Shadow + Jobs + telemetry).

> Scope: this catalogs the **MQTT** topics. Inter-ECU traffic over **CAN** (the
> CANopen Object Dictionary) is a separate fabric — see
> [`can-bus.md`](can-bus.md). Several services expose CAN/OD signals *onto* MQTT
> (e.g. `power` re-publishes the Panasonic pack); those appear here as the MQTT
> topic, with a note.

Ground-truthed from the service binaries in Ghidra (`/S5-v1.5/OS/*`, string +
publish/subscribe xref analysis) and the per-service decode docs. Entries marked
**(inferred)** or `?` are not fully confirmed at the instruction level.

---

## How the bus is used

- **Broker:** `mosquitto` on `localhost`. Default MQTT port `1883`; the shared
  `common::IMQTTClient` wrapper connects with a per-service client-id and a
  keepalive of 60 s. (The `monitor` service's env was seen constructing its
  client against `localhost:5000` — an alternate/inproc broker port; `1883` is
  the system default.)
- **Framework:** every C++ service is built on the same `common` layer
  (`IMQTTClient`, `StateClient`, `Timer`, `Clock`). A publish call is
  `publish(topic, payload, qos, retain)`; a subscribe registers a topic +
  callback. Payloads are usually a **single scalar formatted as text**
  (an int/float/bool) or a small **JSON** object (nlohmann-json); a few are raw
  bytes (SMP/FTP transfers).
- **Retain convention:** *state* and *version/identity* topics are published
  **retained** (so a late subscriber — including the `gateway` mirror — sees the
  current value); *telemetry/event* topics are non-retained.
- **Direction** below is from the named service's point of view: `pub` =
  it publishes, `sub` = it subscribes, `both` = both.

### Quick how-to (on the bike, over the local broker)

```sh
# watch everything
mosquitto_sub -h localhost -t '#' -v

# watch one namespace
mosquitto_sub -h localhost -t 'power/#' -v
mosquitto_sub -h localhost -t 'ux/lock/#' -v

# read a retained value (state / version)
mosquitto_sub -h localhost -t 'power/state' -v          # retained: current state
mosquitto_sub -h localhost -t 'tracking/state' -v       # retained: OFF/AUTO/THEFT

# drive an actuator (publish a command)
mosquitto_pub -h localhost -t 'ux/lock/unlock'   -m 'true'
mosquitto_pub -h localhost -t 'power/state/set'  -m '1'
mosquitto_pub -h localhost -t 'eshifter/gear/set' -m 't'
```

### Shared namespaces (who owns what)

- **`device/<name>/…`** — sub-ECU identity/health. **`monitor`** is the authority
  (publishes `device/<name>/version/{firmware,bootloader,vendor}` and
  `device/<name>/status`); **`update`** *subscribes* to these to decide what to
  flash; **`ride`** publishes the `device/motor_control/*` subset and **`power`**
  the `device/charger/*` subset. `<name>` ∈ the manifest device set
  (`motor_control`, `motor`, `charger`, `ble`, `modem`/`modem_nordic_stack`,
  battery suppliers, …).
- **`ux/*`** — owned by `ux`; consumed by `gateway` (mirror), `tracking`
  (`ux/alarm/state`), and others.
- **`power/*`** — owned by `power`; consumed by `ride` (SOC gate, mostly via CAN),
  `ux`, `gateway`, `update`.
- **`modem/*`, `ble/*`** — produced by the SPI bridges (`spi-mqtt-bridge ble` /
  ` modem`) and consumed by `gateway`/`tracking`/`update`/`ux`.
- **`$aws/…`** — AWS-reserved, only the `gateway` speaks these (to the cloud).

---

## gateway

Local-bus ↔ **AWS IoT** bridge (Go). It subscribes broadly to the local bus and
forwards a curated set to the cloud, and applies cloud Shadow/Jobs back down.

### AWS IoT (cloud side)

| Topic | Dir | Payload / format | Purpose |
| --- | --- | --- | --- |
| `$aws/things/<thing>/shadow/get` | pub | — | request the device shadow (inferred) |
| `$aws/things/<thing>/shadow/get/accepted` | sub | JSON shadow (desired/reported/delta) | shadow snapshot |
| `$aws/things/<thing>/shadow/update` | pub | JSON state doc | report device state to the shadow |
| `$aws/things/<thing>/shadow/update/accepted` | sub | JSON shadow | update confirmed |
| `$aws/things/<thing>/shadow/update/rejected` | sub | JSON error | update rejected |
| `$aws/things/<thing>/jobs/notify` | sub | JSON | jobs queue changed (inferred) |
| `$aws/things/<thing>/jobs/notify-next` | sub | JSON job doc | next queued job |
| `$aws/things/<thing>/jobs/$next/get` | pub | — | fetch next job (inferred) |
| `$aws/things/<thing>/jobs/$next/get/accepted` | sub | JSON job doc | next-job response |
| `$aws/things/<thing>/jobs/+/update` | pub | JSON `{status: IN_PROGRESS/SUCCEEDED/FAILED}` | report job execution status |
| `$aws/things/<thing>/jobs/+/update/accepted` | sub | — | job-update confirmed (inferred) |
| `$aws/things/<thing>/jobs/+/update/rejected` | sub | JSON error | job-update rejected (inferred) |
| `$aws/rules/telemetry/+` | pub | **CBOR** telemetry batch | push batched telemetry to the IoT rules engine |

### Local bus (subscriptions it mirrors / acts on)

| Topic | Dir | Payload / format | Purpose |
| --- | --- | --- | --- |
| `ble/connections/handle/+` | sub | — | BLE connection handle status |
| `ble/proxy/config` | both | JSON | BLE proxy configuration |
| `device/+/serial` | sub | string | per-device serial |
| `device/+/version/firmware` | sub | string | per-device firmware version |
| `error` / `error/#` | sub | — | error topic + subtree |
| `eshifter/state` | sub | JSON | e-shifter state |
| `modem/info/device` | sub | JSON | modem device info |
| `modem/info/network` | sub | JSON | modem network info (inferred) |
| `modem/location/+` | sub | JSON/float | modem geolocation |
| `modem/sms` | sub | — | inbound SMS events (inferred) |
| `power/battery/+/info/{soc,health,cycles}` | sub | uint | battery metrics |
| `power/low_power` | sub | bool | low-power flag |
| `power/state` | sub | string enum | power state |
| `ride/boost` | sub | bool | boost active |
| `ride/info/{speed,pedal_rpm,motor_current,motor_temperature,distance}` | sub | float/uint | ride telemetry |
| `settings/+` | sub | — | settings subtree |
| `telemetry-config` | sub | JSON | telemetry collection config (inferred) |
| `tracking/state` | sub | string OFF/AUTO/THEFT | theft state |
| `ux/alarm/state` | sub | uint 0–3 | alarm escalation |
| `ux/button` | sub | — | button events |
| `ux/info/distance` | sub | float | odometer |
| `ux/lock/#` | sub | — | lock subtree |
| `ux/sensor/temperature` | sub | float | ambient temp |
| `ux/tracking/#` | sub | — | Apple Find-My subtree |

Wildcards: `+` = one level (per-device/per-battery/per-handle), `#` = whole
subtree (`error/#`, `ux/lock/#`, `ux/tracking/#`).

---

## power

Battery + charger + power-state machine. Publishes its state and battery
telemetry retained; subscribes to commands and charger/modem/update signals.
(`battery_primary_*`, `power_control_*`, `IsPrimaryInserted` are **CAN/OD**
overlays bridged via the `vm` layer, listed here where they surface as topics.)

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `power/state` | pub | yes | int enum | operational/charging/standby/low-power/deep-sleep/shipping |
| `power/state/set` | sub | — | int enum | external state request |
| `power/state/status` | pub | no | int (0 ok / 1 retry / 2 active / 3 denied) | state-change result |
| `power/state/extend_timeout` | sub | — | int seconds | extend the current state timeout |
| `power/low_power` | pub | no | int (1 or 2) | low-power phase indicator |
| `power/low_power_extend` | sub | — | int seconds | extend standby timer |
| `power/deep_sleep` | pub | no | bool | deep-sleep entry |
| `power/battery/lipo/info/{soc,capacity,capacity_lvl,voltage,current_now,charge_full,charge_full_design,cycles,health,status,temp,pwr_avg}` | pub | no | int/string | internal LiPo gauge (TI bq27542 sysfs) |
| `power/battery/primary/info/{voltage,charge_voltage,charge_current,discharge_current,max_current,soc,soc_app,health,cycles}` | pub | no | int/u16 | primary Panasonic pack (CAN node 0xA4) |
| `power/battery/primary/info/temperature` | pub | no | JSON `{cell_1,cell_2,chg_mos,dsg_mos}` | pack temperatures |
| `power/battery/primary/info/power` | pub | no | double (W) | pack power = (I/1000)·(V/1000) |
| `device/charger/connected` | pub | no | bool | external charger present |
| `device/charger/{voltage,current}` | both | no | numeric | charger output (monitor polls) |
| `device/charger/mode` | sub | — | string `Success`/`FailRetry`/`InProgress` | charger mode (bg-update flag) |
| `device/charger/finished` | sub | — | bool | charger op finished |
| `eshifter/state` | sub | — | JSON `{current_gear}` | e-shifter state → calibration FSM |
| `eshifter/gear/set` | pub | no | char gear (`'t'`=0x74) | e-shifter calibration request |
| `eshifter/last_calibrated` | both | no | gear value | last calibrated gear |
| `maintenance/battery/primary/reset` | sub | — | — | reset primary pack → STANDBY |
| `modem/system/time` | sub | — | JSON `{"ret":1,"time":<epoch>}` | set RTC from modem in standby |
| `modem/vars/update` | pub | no | binary snapshot | modem variable snapshot (50 ms) |
| `update/progress` | sub | — | numeric | update progress (consulted) |
| `update/background_update/progress_info/charger` | sub | — | JSON `{device,…}` | charger bg-update progress |
| `update/stage2/device_update_started` | sub | — | — | device update started |

---

## ride

Pedal-assist controller. Most motor/pedal data moves over **SSP serial + CANopen
OD**; the MQTT surface is config in / telemetry + motor identity out.

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `settings/assist_level` | sub | ? | uint8 0–4 | assist level (0=off, 1–4 progressive) |
| `settings/region` | sub | ? | uint8 0–4 | region → assist-curve map |
| `ride/boost` | sub | ? | uint8 0/1 | boost mode |
| `ride/info/speed` | pub | ? | uint16 | wheel speed |
| `ride/info/pedal_rpm` | pub | ? | uint16 | pedal cadence |
| `ride/info/motor_temperature` | pub | ? | int16 | motor controller temp |
| `ride/info/torque` | pub | ? | int16 | pedal torque (filtered) |
| `ride/info/motor_speed` | pub | ? | uint16 | motor rotor speed |
| `ride/info/motor_current` | pub | ? | uint16 | motor phase current (EMA) |
| `device/motor_control/status` | pub | ? | uint16 status word | motor controller status + driver bits |
| `device/motor_control/version/{firmware,bootloader,vendor}` | pub | ? | string | motor identity (shared `device/*` namespace) |

> Subscriptions are the three in `ride_app_run()` (`settings/assist_level`,
> `settings/region`, `ride/boost`). The SOC gate (`SOC ≥ 13`) is read from the
> **CANopen OD**, not an MQTT `power/*` subscription. `ride/info/distance` /
> `…/calories` named in `ride.md` were not found as strings in this build.

---

## ux

UX orchestrator. Owns `ux/*`; subscribes to power/ride/ble/modem/tracking inputs.

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `ux/power` | sub | ? | bool | orchestrator on/off |
| `ux/lock/info/state` | pub | yes | enum locked/unlocking | lock state announcement |
| `ux/lock/unlock` | sub | ? | bool | remote unlock trigger |
| `ux/lock/software_lock` | sub | ? | — | auto-relock signal |
| `ux/lock/touch_unlock_request` | sub | ? | — | BLE touch-unlock initiation |
| `ux/lock/touch_unlock_activate` | sub | ? | — | BLE touch-unlock activation (post-auth) |
| `ux/lock/locking_while_riding` | sub | ? | — | lock feedback during motion |
| `ux/alarm/state` | pub | yes | uint8 (1=Alarm2, 2=Alarm3, 3=Theft) | theft escalation level |
| `ux/sound/play` | pub | ? | sound id | speaker command |
| `ux/light/mode` | pub | ? | uint8 | LED-ring mode (theme/pattern) |
| `ux/info/distance` | pub | ? | uint32 (m) | cumulative ride distance (odometer) |
| `ux/sensor/{air,humidity,light,temperature}` | sub | ? | sensor reading | environmental samples (hysteresis) |
| `ux/button` | pub | ? | JSON `{pressed,duration_ms}` | power-button events |
| `info/ecu_serial` | pub | yes | string | device hardware serial |
| `info/bike_id` | pub | yes | string | bike identity |
| `info/sku` | pub | yes | string | SKU / product variant |
| `power/deep_sleep` | sub | ? | — | low-power signal |
| `power/state` | sub | ? | enum | power state |
| `power/state/status` | sub | ? | string | power status detail |
| `power/low_power_extend` | sub | ? | bool | extended low-power |
| `power/battery/primary/info/power` | sub | ? | W | battery power draw |
| `power/battery/primary/info/soc_app` | sub | ? | % | battery SOC (app-visible) |
| `ride/info/speed` | sub | ? | m/s | motor speed |
| `ride/info/power` | sub | ? | W | motor power |
| `ride/info/distance` | sub | ? | uint32 | motor odometer (absolute) |
| `ride/brake_level` | sub | ? | 0–255 | brake sensor |
| `ride/boost` | sub | ? | bool | boost toggle |
| `ble/connections/handle/{id}` | sub | ? | — | BLE connection lifecycle |
| `ble/connections/info` | sub | ? | JSON `{authenticated,…}` | BLE link state |
| `ble/findmy/{certified,control,toggle}` | both | ? | JSON/bool | Apple Find-My provisioning & control |
| `update/ux/finished` | sub | ? | bool | firmware update completion |
| `update/stage` | sub | ? | string | OTA progress stage |
| `tracking/alarm/imu/triggered` | sub | ? | bool | shock-alarm from tilt sensor |

---

## tracking

Anti-theft state machine (OFF/AUTO/THEFT), fed by IMU/SMS/cellular.

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `tracking/state` | both | yes | int 0=OFF / 1=AUTO / 2=THEFT | central state; subscribed at boot to restore, published on transition |
| `ux/alarm/state` | sub | ? | int (acts on ==3) | IMU movement trigger → raise to AUTO |
| `ux/tracking/alarm/imu/triggered` | sub | ? | bool/int | movement subscription → arm 30-min stationary timeout |
| `modem/sms` | sub | ? | JSON `{tracking: enabled/disabled}` | SMS command: enabled→THEFT, disabled→OFF |
| `modem/info/device` | sub | ? | JSON string | modem device info (logged) |
| `modem/location/cellular` | sub | ? | JSON cell fix (MCC/MNC/LAC/CID) | CellLocator input (5-min poll, logged) |
| `ux/sound/play` | pub | no | string filename | alarm sound on theft event |

> SMS command codes: `0` enabled→THEFT, `1` disabled→OFF, `0xFFFFFFFF` parse fail.
> No outbound SMS / no location *estimate* here — that is the modem's job.

---

## monitor

Component health/version supervisor (see [`monitor.md`](monitor.md)). Publishes
per-component identity/health; aliveness from CAN heartbeats + modem ping.

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `device/<name>/version/firmware` | pub | yes? | string | sub-ECU firmware version (authority) |
| `device/<name>/version/bootloader` | pub | yes? | string | sub-ECU bootloader version |
| `device/<name>/version/vendor` | pub | yes? | string | sub-ECU vendor/supplier version |
| `device/<name>/status` | pub | yes? | string | sub-ECU presence/health |
| `device/motor/status` | pub | yes? | string `ok`/`error` | LPC MotorController (CAN node 0xA1) health |
| `monitor/component/charger/type` | pub | no | string | detected charger supplier tag |
| `ble/heartbeat` | pub | no | `"1"` | BLE-component liveness beacon |
| `ble/system/version_info` | pub | yes? | string | BLE firmware version |
| `ble/system/reset_reason` | pub | yes? | string | BLE last reset reason |
| `ble/vars` | pub | no | JSON | BLE reported variables |

> `<name>` carries `motor_control` (version topics) and `motor` (status), plus
> the other supervised sub-ECUs. Heartbeat *aliveness* arrives over **CAN**
> (CANopen heartbeat, 2 s) — not MQTT; the modem's aliveness is an ICMP **ping**
> to `1.1.1.1`/`8.8.4.4`. The BLE `ble/*` topics overlap the bridge namespace.

---

## update

Peripheral + system OTA. **Subscribes** to the `device/<name>/*` versions
`monitor` publishes (to decide what to flash) and drives SMP/FTP transfers for
the Nordic parts; CAN sub-ECUs are flashed off-bus (page+CRC).

| Topic | Dir | Retain | Payload / format | Purpose |
| --- | --- | --- | --- | --- |
| `update/start` | sub | no | JSON `{status,version}` | trigger an update |
| `update/current_version` | pub | no | semver | installed system version (at boot) |
| `update/update_available` | pub | no | JSON `{status,version}` | newer version detected |
| `update/progress` | pub | yes | numeric 0–100 / JSON `{stage,current,total,status}` | update progress |
| `update/finished` | pub | yes | numeric / JSON `{status,error_code?}` | final result (retained) |
| `update/ux/finished` | pub | yes | numeric | UX-layer completion |
| `update/stage` | pub | no | numeric (1/2) | current stage |
| `update/stage2/device_update_started` | pub | no | device name | stage-2 per-device start |
| `update/user_button_pattern/roll_back` | sub | no | ? | user-triggered rollback |
| `update/background_update/+` | sub | yes | status/config | background-update management |
| `update/background_update/progress_info/+` | sub | yes | JSON progress | per-component bg progress |
| `update/background_update/finished/` | pub | yes | status | bg-update finished |
| `update/background_update/modem_nordic_stack` | pub | no | string/numeric | Nordic stack update progress |
| `device/<device>/status` | sub | yes | numeric/JSON | per-device presence (from `monitor`) |
| `device/<device>/version/firmware/#` | sub | yes | semver | sub-ECU firmware (from `monitor`) |
| `device/<device>/version/bootloader/#` | sub | yes | semver | sub-ECU bootloader |
| `device/<device>/version/vendor/#` | sub | yes | semver | sub-ECU vendor (battery/charger suppliers) |
| `modem/nordic/version_info` | both | no | JSON | modem version query/response (cleared after flash) |
| `modem/nordic/update/config` | pub | no | JSON | SMP/MCUmgr update config → modem |
| `modem/config/lte` | pub | no | JSON | LTE config before modem update |
| `modem/ftp/command` / `modem/ftp/reply` | pub / sub | no | SMP/FTP bytes | modem file transfer |
| `modem/system/reboot` | pub | no | — | reboot modem after flash |
| `ble/ftp/firmware/command` / `ble/ftp/firmware/reply` | pub / sub | no | SMP/FTP bytes | BLE (nRF52) file transfer |
| `ble/system/reboot` | pub | no | — | reboot BLE after flash |
| `ftp_server/command` / `ftp_server/reply` / `ftp_server/file_finished` | pub / sub / sub | no | SMP/FTP | generic FTP transfer (also `mqtt-ftp-service`) |
| `phone/ftp_server/command` / `phone/ftp_server/reply` | pub / sub | no | SMP/FTP | phone-side FTP transfer |
| `power/low_power_extend` | pub | no | numeric s | hold off low-power during update |
| `power/state/status` | sub | no | enum | power state (consulted before flashing) |

> **`lightweight_update`** (the standalone CLI flasher, pkg
> `vmxs5-embedded-lightweight-update`) reuses this same machinery: it connects as
> client-id `lightweight_update` / user `update-service` to `localhost:5000` and
> subscribes `device/+/version/{firmware,bootloader,vendor}/#` to version-check
> the one device it flashes. See [`../lightweight_update/`](../lightweight_update/).

---

## Not on this bus

`mqtt-ftp-service` rides the `ftp_server/command`+`ftp_server/reply` channel
(rooted at `/tmp`) for log/cert/config push-pull. `logging` pulls `error`/log
topics into `/var/log`. The CAN bridges (`spi-can-if-linux`, `imx8_bridge`) and
the SPI bridges (`spi-mqtt-bridge ble`/`modem`) translate non-IP links onto the
bus — the `ble/*` and `modem/*` namespaces above originate there.
