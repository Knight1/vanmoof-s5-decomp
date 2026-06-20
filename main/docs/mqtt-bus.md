# The MQTT bus — internal IPC + BLE access control

The whole bike is wired together over one **mosquitto 1.6.7** broker on
loopback. Config (`vmxs5-embedded-mosquitto-conf`):

```
listener 1883
bind_interface lo                                  # internal only, never off-box
acl_file /etc/mosquitto/acl                         # per-user topic allow-lists
persistence true
persistence_location /run/media/mmcblk2p6/config/mosquitto/   # retained msgs survive reboot
```

There is **no `password_file` and no auth plugin** — only an `acl_file`. So the
broker accepts **anonymous** connections (mosquitto 1.6's default) and enforces
the ACL purely by the **username the client supplies in CONNECT**; there is no
secret verifying that username. The entire trust model therefore rests on
(a) the listener being bound to `lo` (only on-box processes can connect) and
(b) the BLE/modem bridges choosing the right username on a remote party's
behalf. Any process that reaches `127.0.0.1:1883` can claim `factory` (full
`readwrite #`) — the isolation is the loopback bind, not a credential.

Two classes of usernames appear in the ACL:

1. **Service users** — the on-box daemons (`power-service`, `ux-service`,
   `gateway`, …). Most have full `readwrite #`; `gateway` is deliberately
   constrained (read-all + a fixed write-list).
2. **BLE rider roles** — `ble-role-N`. When a phone connects over BLE, the
   nRF52 authenticates the phone's **certificate**, derives its role number,
   and the BLE proxy then speaks to the broker as `ble-role-N`, so the rider's
   app publishes/subscribes **through the ACL for that role**. The real
   authentication is the BLE certificate check on the nRF52 (the broker does
   none); the `ble-role-N` username is just the post-auth label. (VanMoof's own
   doc: Jira `AD/2321186824 — RFC Certificate for SA5 Bluetooth
   authentication`, referenced in the ACL header.)

## BLE rider roles (`ble-role-N`)

| Role | Name | Access |
| --- | --- | --- |
| `ble-role-7` | **Owner** | Curated allow-list: full `settings/#` + `update/#`; lock/unlock; read ride/power/SoC/state, eshifter, errors, `ble/findmy/*`, `device/+/status`; `ftp_server` command/reply; `ble/proxy`. |
| `ble-role-11` | **Shared bike** | Like Owner but `settings` limited to a safe subset (assist level, shift levels, lights, bell, brake/turn lights, ride animation) — no region/mode/factory settings. |
| `ble-role-17` | **Bike doctor** | `readwrite #` (full bus) |
| `ble-role-22` | **Bike Hunter** | `readwrite #` (full bus) |
| `ble-role-55` | **QA Engineer** | `readwrite #` (full bus) |
| `ble-role-66` | **R&D Engineer** | `readwrite #` (full bus) |

> Roles 17/22/55/66 are privileged/internal (service, theft-recovery,
> test, engineering) and get unrestricted bus access; the customer-facing
> roles (7 Owner, 11 Shared) are tightly scoped. The numeric role IDs come from
> the BLE auth certificate.

## Link-controller users

| User | Access |
| --- | --- |
| `ble-ctrl` | `ble/#`, `device/ble/#`, `logging/event/ble`; reads `power/#`, `info/#`, modem time. The `spi-mqtt-bridge@ble` identity. |
| `modem-ctrl` | `modem/#`, `device/modem/#`; reads `power/#`, `ble/vars/update`. The `spi-mqtt-bridge@modem` identity. |
| `gateway` | read `#`; writes a fixed control set (see below). |
| `factory` | `readwrite #` — provisioning / line linker. |
| `power/ux/ride/update/monitor/logging/tracking-service`, `mqtt-ftp-server` | `readwrite #`. |

`gateway` write-list (the commands the cloud is allowed to inject):
`power/state/set`, `settings/{region,mode,assist_level,shift_levels,light_mode,
light_auto_threshold,ride_animation_right,brake_lights,turning_lights,
animation_theme,bell_sound}/set`, `ux/sound/play`,
`ux/usb/settings/enable/set`, `eshifter/gear/set`, `update/start`.

## Topic namespace

| Prefix | Owner service | Examples |
| --- | --- | --- |
| `power/state…` | `power` | `power/state` (+ `/set`, `/status`, `/extend_timeout`), `power/deep_sleep`, `power/low_power` (+ `_extend`) — the 8-state machine (INVALID/SHIPPING/STANDBY/OPERATIONAL/CHARGING/UPDATING/ALARM/MAINTENANCE) |
| `ride/…` | `ride` | ride telemetry / control |
| `ux/lock/…` | `ux` | `unlock`, `touch_unlock_activate`, `touch_unlock_request`, `info/state`, `locking_while_riding` |
| `ux/info/…`, `ux/sensor/…`, `ux/sound/play`, `ux/usb/settings/enable/set` | `ux` | UI info, sensors, sounds, USB-C charge enable |
| `settings/…` | `ux`/config | `assist_level`, `shift_levels`, `light_mode`, `light_auto_threshold`, `bell_sound`, `brake_lights`, `turning_lights`, `ride_animation_right`, `animation_theme`, `region`, `mode` (each `…/set`) |
| `eshifter/gear`, `eshifter/gear/set` | e-shifter (via CAN) | current / requested gear |
| `power/battery/primary/info/…` | `power` | Panasonic pack (decoder-confirmed, from CAN): `soc`, `soc_app`, `voltage`, `charge_voltage`, `charge_current`, `discharge_current`, `max_current`, `temperature` (JSON: cell 1/2, chg/dsg mos), `power`, `health`, `cycles` |
| `power/battery/lipo/info/…` | `power` | internal LiPo (I²C bq27542 gauge): `soc`, `voltage`, `current_now`, `temp`, `capacity` (+ `_lvl`), `charge_full` (+ `_design`), `cycles`, `health`, `pwr_avg`, `status` |
| `device/charger/…` | charger → `power` | external LiteON charger status (via CAN/bridge): `connected`, `voltage`, `current`, `mode`, `finished` |
| `maintenance/battery/primary/reset` | `power` | reset the primary pack |
| `device/+/version/{firmware,bootloader,vendor}/#`, `device/+/status` | `update` | per-ECU version/status (drives OTA) |
| `update/…`, `update/start` | `update` | OTA progress / trigger (see [`update.md`](update.md)) |
| `ble/…` | BLE side | `ble/proxy`, `ble/proxy/config`, `ble/findmy/report`, `ble/findmy/certified`, `ble/system/version_info`, `ble/vars/update` |
| `modem/…` | modem side | `modem/info/datetime`, `modem/system/time` (→ RTC), `modem/vars/update`, `modem/config/lte`, `modem/ftp/{command,reply}`, `modem/nordic/{update/config,version_info}`, `modem/system/reboot` |
| `device/+/status`, `device/ble/#`, `device/modem/#` | per-device status | |
| `error/#`, `info/#`, `logging/event/…` | logging/monitor | error & event reporting |
| `ftp_server/command`, `ftp_server/reply` | `mqtt-ftp-service` | file transfer channel |

The `power/…`, `power/battery/…`, `device/charger/…` and `maintenance/…` topics
above are **decoder-confirmed** from the reversed `power` service. Its **complete
catalog** (19 subscribe + 42 publish, with the handler/publisher function and
payload of every topic) is in **[`../power/mqtt.md`](../power/mqtt.md)**; the CAN
side is in [`can-bus.md`](can-bus.md).

### Find My

`ble/findmy/report` and `ble/findmy/certified` are the **Apple Find My**
glue: the nRF52 BLE app (see [`ble/`](../../ble/)) runs the Find My accessory
network role, and the report/certified state is surfaced on the bus for the
Owner/Shared roles and forwarded to the cloud by `gateway`.

## Gateway ↔ AWS IoT bridge (binary-confirmed)

The `gateway` ACL line above is the broker-side *permission* envelope. What the
service actually bridges is now pinned from its Go symbol table (the
`internal/*` packages — see [`../gateway/README.md`](../gateway/README.md)).
Direction, package and topics:

| Dir | Local bus (mosquitto `lo:1883`) | gateway path | AWS IoT (mTLS, `$aws/things/<thing>/…`) |
| --- | --- | --- | --- |
| **bus → cloud** | reads metric topics (`telemetry.Collector.setSubscriptions` / `handleMessage`, gated by `ParseConfig`/`IgnoreTopic`) | buffer → `telemetry.Batch.MarshalBinary` (**CBOR**) → `telemetry.Router.Publish`, routed over **modem (LTE-M)** or **BLE proxy** per `TransportMode` → `iot.Client.Publish` | telemetry rule topic (`$aws/rules/…`) |
| **bus → cloud** | bike/device reported state | `iot/shadow.Client.Report` (reported state, `State.MarshalJSON`) | `…/shadow/name/<n>/update` (named shadow) |
| **bus → cloud** | `bike_id` (`/run/media/mmcblk2p6/bike_id`) | `gateway.ConfigShadow.syncBikeID` / `setBikeID` ("Reporting/Updating bike id", "Bike id is in sync") | shadow (named) |
| **cloud → bus** | telemetry-config + bike_id only | desired shadow → `iot.Client.DeviceShadow` / `shadow.Client.State` → `gateway.ConfigShadow.Sync` → `telemetry.Collector.SetConfig` (`telemetry-config`, "set initial telemetry config") | `…/shadow/name/<n>/get|update/accepted` |
| **cloud → bus** | **none beyond the above** via Jobs | `iot/job` → single op **`log_upload`** (HTTP file upload, does **not** publish to the bus) — see gateway README | `…/jobs/notify-next`, `…/jobs/<id>/update` |
| **bus ↔ local** | `ble/proxy(/config)`, BLE-auth, timezone | `event.HandleBLE` / `HandleProxyConfig` / `HandleTimezone`, `ble.Proxy` — also feeds `TransportMode` (`prefer-modem`/`prefer-proxy`/`auto-disconnect`) | (not forwarded) |

So in this build the gateway's cloud→bus surface is **declarative** (shadow
desired → telemetry config + bike id) plus the single `log_upload` job — it does
**not** translate cloud messages into the `power/state/set` / `settings/*/set` /
`eshifter/gear/set` / `update/start` **command** writes. Those `…/set` topics are
*granted* to `gateway` by the ACL (the designed control envelope) but are driven
on-bike by the BLE/app roles (Owner/Shared) and the `update` service; the gateway
holds the write permission without exercising it here. The AWS endpoint + device
identity come from `mmcblk2p6/config.cfg` (`bike.readEndpoint`), trust-anchored to
two Amazon roots (`iot/ca.CertPool`).

## Security posture (notes)

- The broker has **no passwords/auth** — anonymous + ACL-by-claimed-username.
  Combined with the loopback bind and an **empty** `iptables.rules`, the only
  thing stopping a process from claiming `factory` (full bus) is that it must
  already be running on the box. Off-box reach is only via `gateway` (cloud,
  mTLS) and the BLE/modem bridges.
- The privileged BLE roles (Bike Hunter/QA/R&D/doctor) get full `readwrite #`;
  possession of the corresponding BLE auth certificate grants total bus
  control. The trust boundary is the **BLE certificate check on the nRF52**, not
  the broker (which only maps the post-auth role→ACL).
- `root` in `/etc/shadow` has a real `$6$` sha512crypt hash (not blank).
- `sshd` is installed but not enabled in this production image.
