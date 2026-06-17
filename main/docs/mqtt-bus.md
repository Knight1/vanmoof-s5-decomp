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

Two classes of MQTT users authenticate to the broker:

1. **Service users** — the on-box daemons (`power-service`, `ux-service`,
   `gateway`, …). Most have full `readwrite #`; `gateway` is deliberately
   constrained (read-all + a fixed write-list).
2. **BLE rider roles** — `ble-role-N`. When a phone connects over BLE and
   authenticates, the BLE SoC maps its certificate role to a `ble-role-N` MQTT
   identity, so the rider's app effectively publishes/subscribes on the bus
   **through the ACL for their role**. This is the bike's authorization model.
   (VanMoof's own doc: Jira `AD/2321186824 — RFC Certificate for SA5 Bluetooth
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
| `power/…` | `power` | `power/state`, `power/state/set`, `power/battery/primary/info/soc`, `…/soc_app` |
| `ride/…` | `ride` | ride telemetry / control |
| `ux/lock/…` | `ux` | `unlock`, `touch_unlock_activate`, `touch_unlock_request`, `info/state`, `locking_while_riding` |
| `ux/info/…`, `ux/sensor/…`, `ux/sound/play`, `ux/usb/settings/enable/set` | `ux` | UI info, sensors, sounds, USB-C charge enable |
| `settings/…` | `ux`/config | `assist_level`, `shift_levels`, `light_mode`, `light_auto_threshold`, `bell_sound`, `brake_lights`, `turning_lights`, `ride_animation_right`, `animation_theme`, `region`, `mode` (each `…/set`) |
| `eshifter/gear`, `eshifter/gear/set` | e-shifter (via CAN) | current / requested gear |
| `update/…`, `update/start` | `update` | OTA progress / trigger |
| `ble/…` | BLE side | `ble/proxy`, `ble/proxy/config`, `ble/findmy/report`, `ble/findmy/certified`, `ble/system/version_info`, `ble/vars/update` |
| `modem/…` | modem side | `modem/info/datetime`, `modem/system/time`, … |
| `device/+/status`, `device/ble/#`, `device/modem/#` | per-device status | |
| `error/#`, `info/#`, `logging/event/…` | logging/monitor | error & event reporting |
| `ftp_server/command`, `ftp_server/reply` | `mqtt-ftp-service` | file transfer channel |

### Find My

`ble/findmy/report` and `ble/findmy/certified` are the **Apple Find My**
glue: the nRF52 BLE app (see [`ble/`](../../ble/)) runs the Find My accessory
network role, and the report/certified state is surfaced on the bus for the
Owner/Shared roles and forwarded to the cloud by `gateway`.

## Security posture (notes)

- The broker is loopback-only; there is **no network firewall** doing the
  isolation (`/etc/iptables/iptables.rules` is empty). Off-box reach is only
  via `gateway` (cloud, mTLS) and the BLE/modem bridges.
- The privileged BLE roles (Bike Hunter/QA/R&D/doctor) get full `readwrite #`;
  possession of the corresponding BLE auth certificate grants total bus
  control. The trust boundary is the **BLE certificate role**, enforced by the
  nRF52 auth flow, not by the broker beyond the role→ACL mapping.
- `root` in `/etc/shadow` has a real `$6$` sha512crypt hash (not blank).
- `sshd` is installed but not enabled in this production image.
