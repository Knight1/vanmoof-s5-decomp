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

- **Broker:** `mosquitto` on `localhost:1883`, **bound to `lo`** (loopback only —
  the bus is never exposed off-device). The shared `common::IMQTTClient` wrapper
  hardcodes the connect to `localhost:1883` keepalive 60 s (`0x75b`/`0x3c`) with a
  per-service client-id. Full broker + ACL settings: see **MQTT settings &
  endpoints** at the end of this doc.
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

### Cross-cutting topics (nearly every service)

Confirmed by a full string sweep of all seven service binaries:

- **`logging/event` (+ `logging/event/<src>`)** — published by *every* C++ service
  (`power`, `ride`, `ux`, `tracking`, `monitor`, `update`) and the bridges; the
  **`logging`** service subscribes and persists them to `/var/log`. `ble-ctrl`
  has a dedicated `w logging/event/ble`.
- **`power/state`** + **`power/state/{set,status,extend_timeout}`** — the
  power-state control plane: `power` owns it, and `ride`/`ux`/`tracking`/
  `monitor`/`update` all subscribe `power/state` and may `set`/`extend_timeout`
  (e.g. hold the bike awake during a flash) and read `…/status`.
- The sweep also rejected non-topic artifacts that look topic-ish: Go package
  paths in `gateway` (`telemetry/{batch,collector,router,mode,ignore,config}`,
  `light/singleflight`) are **internal package names, not bus topics**; only
  `$aws/rules/telemetry/+` is a real telemetry topic.

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
| `ride/info/distance` | pub | ? | uint32 | trip distance |
| `ride/info/calories` | pub | ? | uint | estimated calories |
| `ride/brake_level` | pub | ? | 0–255 | brake-sensor level |
| `device/ride/status` | pub | ? | string | ride-service health |
| `device/motor_control/status` | pub | ? | uint16 status word | motor controller status + driver bits |
| `device/motor_control/version/{firmware,bootloader,vendor}` | pub | ? | string | motor identity (shared `device/*` namespace) |
| `power/state` + `power/state/{set,status,extend_timeout}` | sub/pub | — | enum/int | power-state coordination (see cross-cutting note) |

> The config subscriptions are `settings/assist_level`, `settings/region`,
> `ride/boost`. The SOC gate (`SOC ≥ 13`) is read from the **CANopen OD**, not an
> MQTT `power/*` topic. (`ride/info/distance` and `…/calories` ARE present in the
> binary — confirmed by the full string sweep.)

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
| `info/frame_number` | pub | yes | string | frame number |
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
| `ble/connections/handle` | sub | ? | — | BLE connection lifecycle |
| `ble/connections/info` | sub | ? | JSON `{authenticated,…}` | BLE link state |
| `ble/certificate_public_key` | both | ? | bytes | BLE auth public key |
| `ble/findmy/certify` / `ble/findmy/certified` | both | ? | bool | Apple Find-My certification request/state |
| `ble/findmy/control` | both | ? | JSON | Find-My control channel |
| `ble/findmy/provisioned` | pub | ? | bool | Find-My provisioned flag |
| `ble/findmy/report` | sub | ? | bytes | Find-My location-report payload |
| `ble/findmy/serial_lookup` | both | ? | string | Find-My serial lookup |
| `ble/findmy/sound` | sub | ? | — | Find-My "play sound" trigger |
| `ble/findmy/event/{enable,disable,paired,pairing,pairing_failed,reset,reset_ignored}` | pub | ? | — | Find-My pairing/lifecycle events |
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
| `modem/tracking/type/set` | pub | ? | string/int | select the modem's tracking/location mode |
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
| `ble/vars/update` | pub | no | JSON | BLE reported variables |
| `modem/heartbeat` | pub | no | `"1"` | modem-component liveness beacon |
| `modem/system/version_info` | pub | yes? | string | modem firmware version (`mfw_nrf9160_1.3.1`) |
| `modem/system/reset_reason` | pub | yes? | string | modem last reset reason |
| `modem/info/network` | pub | no | JSON | modem network info |
| `modem/config/lte` | pub | no | JSON | LTE config snapshot |
| `modem/vars/update` | pub | no | JSON | modem reported variables |

> The supervisor runs **parallel `ble/*` and `modem/*` component topic sets** —
> it watches the BLE SoC and the Nordic modem the same way. `<name>` carries
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
> client-id `lightweight_update` / user `update-service` to `localhost:1883` and
> subscribes `device/+/version/{firmware,bootloader,vendor}/#` to version-check
> the one device it flashes. See [`../lightweight_update/`](../lightweight_update/).

---

## Not on this bus

`mqtt-ftp-service` rides the `ftp_server/command`+`ftp_server/reply` channel
(rooted at `/tmp`) for log/cert/config push-pull. `logging` pulls `error`/log
topics into `/var/log`. The CAN bridges (`spi-can-if-linux`, `imx8_bridge`) and
the SPI bridges (`spi-mqtt-bridge ble`/`modem`) translate non-IP links onto the
bus — the `ble/*` and `modem/*` namespaces above originate there.

---

# MQTT settings & endpoints

## Local broker (`/etc/mosquitto/mosquitto.conf`)

```
listener 1883
bind_interface lo
acl_file /etc/mosquitto/acl
persistence true
persistence_location /run/media/mmcblk2p6/config/mosquitto/
```

- **Endpoint:** `tcp://localhost:1883`, **bound to `lo` only** — the bus is not
  reachable off-device. No TLS listener, no `password_file`: the broker allows
  anonymous connects and authorises purely by the **ACL keyed on the client's
  username**. Each service connects with a fixed username (`ux-service`,
  `power-service`, `gateway`, …); phone/app sessions are authenticated at the
  **BLE/TLS layer** (the per-role client certificate) and proxied onto the bus
  under a `ble-role-N` username, which the ACL then constrains.
- **Persistence:** retained messages + the in-flight DB live on the ext4 config
  partition (`mmcblk2p6/config/mosquitto/`), so retained state (current
  `power/state`, versions, `settings/*`, `tracking/state`, …) survives reboots.
- `pkg vmxs5-embedded-mosquitto-conf` ships both files.

## Roles & permissions (`/etc/mosquitto/acl`)

### Internal service accounts — full bus (`readwrite #`)
`power-service`, `ride-service`, `ux-service`, `update-service`,
`monitor-service`, `logging-service`, `tracking-service`, `mqtt-ftp-server`,
`factory`. (`update-service` is also the username used by the `update` daemon
**and** the `lightweight_update` CLI.)

### Bridge accounts (scoped)
| User | Grants |
| --- | --- |
| `ble-ctrl` | `rw ble/#`, `rw device/ble/#`, `w logging/event/ble`, `r power/#`, `r info/#`, `r modem/info/datetime`, `r modem/system/time` |
| `modem-ctrl` | `rw modem/#`, `rw device/modem/#`, `r power/#`, `r ble/vars/update` |
| `gateway` | `r #` (mirror everything to cloud) **plus** a fixed **write whitelist** (the only down-commands the cloud may inject): `power/state/set`, `settings/{region,mode,assist_level,shift_levels,light_mode,light_auto_threshold,ride_animation_right,brake_lights,turning_lights,animation_theme,bell_sound}/set`, `ux/sound/play`, `ux/usb/settings/enable/set`, `eshifter/gear/set`, `update/start` |

### BLE phone/app roles (per-certificate; the on-bike permission model)
| Role | Name | Scope |
| --- | --- | --- |
| `ble-role-7` | **Owner** | `rw settings/#`, `rw update/#`, lock control (`w ux/lock/unlock`, `w ux/lock/touch_unlock_activate`, `r …/touch_unlock_request`, `r …/info/state`, `r …/locking_while_riding`), `r ride/#`, `r power/battery/primary/info/{soc,soc_app}`, `r power/state`, `w ftp_server/command` `r ftp_server/reply`, `r ble/proxy` `w ble/proxy/config`, `r ble/findmy/{report,certified}`, `r ble/system/version_info`, `r ux/info/#` `r ux/sensor/#`, `r error/#`, `r eshifter/gear` `w eshifter/gear/set`, `r device/+/status` |
| `ble-role-11` | **Shared bike** | like Owner but `settings` is **per-key** only: `rw settings/{assist_level,shift_levels,light_mode,light_auto_threshold,bell_sound,brake_lights,turning_lights,ride_animation_right}/#` (no blanket `settings/#`), `rw update/#`, same lock/ride/power/ftp/ble subset |
| `ble-role-17` | **Bike doctor** | `rw #` (full) |
| `ble-role-22` | **Bike Hunter** | `rw #` (full) |
| `ble-role-55` | **QA Engineer** | `rw #` (full) |
| `ble-role-66` | **R&D Engineer** | `rw #` (full) |

> Convention: a setting/command is **written** to `<topic>/set`; the owning
> service validates it, applies it, and re-publishes the **current value
> (retained)** to the base `<topic>`. The cloud (`gateway`) and the app
> (BLE roles) only ever write the `…/set` form.

## The `settings/*` namespace (user/app preferences)

Writable via `settings/<key>/set`; current value retained at `settings/<key>`.
Owners read the `/set` topic and apply. Keys seen in the ACL + binaries:

| Setting key | Owner | Meaning |
| --- | --- | --- |
| `settings/region` | ride | region → legal assist-curve cap |
| `settings/mode` | ride/ux | ride/assist mode |
| `settings/assist_level` | ride | pedal-assist level 0–4 |
| `settings/shift_levels` | ux/eshifter | e-shifter gear-shift points |
| `settings/light_mode` | ux | headlight/taillight mode (auto/on/off) |
| `settings/light_auto_threshold` | ux | lux threshold for auto-headlight |
| `settings/bell_sound` | **ux** | **which bell/horn sound the bike plays** (see below) |
| `settings/brake_lights` | ux | brake-light behaviour toggle |
| `settings/turning_lights` | ux | turn-indicator behaviour toggle |
| `settings/ride_animation_right` | ux | LED ride-animation (right) |
| `settings/animation_theme` | ux | LED-ring animation theme |
| `settings/backup_unlock_code` | ux | the 3-symbol `BackupUnlock` kick-code |
| `ux/usb/settings/enable` | ux | enable the USB (phone-charging) port |

## Cloud endpoint — AWS IoT (via `gateway`)

The Go `gateway` is the only bridge off-bike. It connects to **AWS IoT Core**
over **TLS (mTLS)** — device X.509 cert + root-CA **pinning to the two Amazon
roots (CA1 + CA3)**, no `InsecureSkipVerify` (see `gateway-aws-iot`). The ATS
endpoint hostname is **provisioned per-device** (cert/config, not hardcoded in
the binary). Topic roots it speaks: `$aws/things/<thing>/shadow/*`,
`$aws/things/<thing>/jobs/*`, `$aws/rules/telemetry/+` (CBOR) — detailed in the
**gateway** section above.

## `settings/bell_sound/set` — what it does

It selects the bike's **bell (electronic horn) sound**. The bike has a
configurable bell with several built-in clips — `bell_dingdong`,
`bell_partyhorn`, `bell_ping`, `bell_submarine`, plus `bell_custom` (a
user-uploaded sound) — all string ids found in the **`ux`** binary alongside
`bell_sound` and `ux/sound/play`. Flow:

1. A writer publishes the chosen id to **`settings/bell_sound/set`**. Per the
   ACL the writers are the **`gateway`** (cloud/app → has an explicit
   `w settings/bell_sound/set` grant) and the **Shared-bike / Owner BLE roles**
   (`ble-role-11` `rw settings/bell_sound/#`, `ble-role-7` `rw settings/#`) —
   i.e. the phone app over BLE.
2. **`ux`** (the sound/UX owner) consumes it, stores the selection, and
   re-publishes the current value (retained) to `settings/bell_sound`.
3. When the rider rings the bell, `ux` plays the selected clip via
   **`ux/sound/play`**.

So it's the app/cloud setting for *which tone the bike's bell rings* — purely a
UX preference, not a security or motor control path.
