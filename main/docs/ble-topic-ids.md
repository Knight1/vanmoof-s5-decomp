# BLE topic ids — the phone-facing view of the MQTT bus

> How a phone, or any BLE client, reads live bike data — including the CAN
> heartbeats — from an S5/A5 over Bluetooth, and the complete **16-bit topic id
> ⇄ MQTT topic** registry that makes it work.
>
> Companion docs: [`mqtt-topics.md`](mqtt-topics.md) (what each topic *means*,
> payload + retain), [`mqtt-bus.md`](mqtt-bus.md) (broker + per-role ACL),
> [`can-bus.md`](can-bus.md) (the CAN fabric underneath).
> Machine-readable copy of the table: [`ble-topic-ids.json`](ble-topic-ids.json).

## 1. The path a CAN heartbeat takes to the phone

```
sub-ECU (LPC / C2000)         CANopen heartbeat, every 2 s
      │  CAN-FD                (e.g. eshifter 0x01111840, frontlight 0x01111880)
imx8_bridge (LPC55S69)        CAN ⇄ SPI bridge MCU
      │  SPI
spi-can-if-linux  →  vcan0    SocketCAN, 29-bit ids
      │
monitor service               heartbeat → per-ECU aliveness
power / ride services         CAN OD signals → telemetry
      │  MQTT (mosquitto, lo:1883)
      │    device/<ecu>/status, ride/info/*, power/battery/*, …
spi-mqtt-bridge ble           topic string ⇄ 16-bit topic id   ← the table below
      │  SPI frames (connection_id, role, topic, payload)
nRF52840 `ble` firmware       per-connection routing, certificate auth
      │  GATT notify
phone / third-party app       [PayloadType 1][topic:2 BE][CBOR value]
```

**Nothing on the phone side ever sees a raw CAN frame.** What a third-party app
presents as "CAN bus heartbeats" is the `device/<ecu>/status` family — ids
`0x0030`–`0x003e` — which `monitor` derives from each sub-ECU's 2 s heartbeat,
plus the CAN-derived telemetry (`ride/info/*`, `power/battery/*`,
`device/charger/*`, `eshifter/*`).

> `ble/heartbeat` (`0x00f2`) and `modem/heartbeat` (`0x10d1`) are **not** CAN:
> they are the bridge's own MQTT liveness beacons for the two Nordic parts,
> which sit on SPI, not on CAN.

## 2. Transport

| Role | UUID |
| --- | --- |
| Service | `e3d80000-3416-4a54-b011-68d41fdcbfcf` |
| Notify (bike → client) | `e3d80001-3416-4a54-b011-68d41fdcbfcf` |
| Write (client → bike) | `e3d80002-3416-4a54-b011-68d41fdcbfcf` |

Clients fall back to writing on `…0001` when `…0002` is not exposed. The bike's
BLE address is **randomised on every reboot** — scan for the service UUID rather
than pinning a MAC.

### Fragmentation

Every logical message is split into fragments that fit the negotiated MTU:

```
[ (0x80 if first else 0x00) | seq_id ] [ remaining : 2 bytes BIG-endian ] [ chunk ]
```

- `seq_id` starts at 1 and wraps 127 → 1.
- On the **first** fragment `remaining` = total message length − 1 (the body
  bytes after the PayloadType byte).
- The message is complete once the reassembled buffer reaches `remaining + 1`.

### Message layer

`[PayloadType:1][body…]`

| PayloadType | Value | Direction | Body |
| --- | --- | --- | --- |
| `TOPIC_CBOR` | 1 | both | `topic:2 BE` + CBOR value |
| `SUBSCRIBE` | 2 | client → bike | `0xE001` + list of `topic:2 BE` |
| `AUTH_CERT` | 3 | client → bike | certificate (64-byte CA sig + CBOR) |
| `AUTH_CHALLENGE` | 4 | both | bike: 16-byte nonce / client: 64-byte sig |
| `AUTH_DETAILS` | 5 | bike → client | CBOR `{enc: bool, auth: bool}` |
| `MTU_UPDATE` | 6 | — | — |
| `PARAM_UPDATE` | 7 | bike → client | CBOR `{latency, timeout, interval}` |

## 3. Authentication

```
bike → client   AUTH_DETAILS   {enc: false, auth: false}
client → bike   echo it back verbatim
client → bike   AUTH_CERT      64-byte CA signature ‖ CBOR payload
bike → client   AUTH_CHALLENGE 16-byte nonce
client → bike   AUTH_CHALLENGE Ed25519 signature over exactly those 16 bytes
bike → client   AUTH_DETAILS   {enc: false, auth: true}
```

Certificate CBOR map: `i` cert id, `f` frame number, `b` bike serial, `e` expiry
(unix s), `r` **role**, `u` user UUID (16 B), `p` Ed25519 public key (32 B).
Certificates are typically valid ~7 days.

The firmware side of this is `auth_parse_certificate_challenge` (`0x3d920`) and
`auth_send_disconnect_reason` (`0x3e4b0`) in [`../../ble/`](../../ble/); a
rejected login disconnects with one of `invalid certificate/parsing error`,
`server signature invalid`, `bike id invalid`, `certificate expired`,
`certificate blacklisted`, `challenge return invalid`, `authentication timeout`.

## 4. Subscribing

Write a `SUBSCRIBE` message listing the ids you want:

```
02  E0 01  00 30  00 31  00 33  00 39 …
│   │      └── device/ble/status, device/battery_primary/status, …
│   └── control topic 0xE001
└── PayloadType 2
```

`0xE001` is literally the MQTT topic **`bridge/subscribe`**: `spi-mqtt-bridge`
looks each id up in the table below and subscribes to that topic string *on your
connection's behalf* (logging `Already subscribed to %s` on a duplicate), then
pushes every matching message back to your connection as a `TOPIC_CBOR`
notification. `0xE002` = `bridge/unsubscribe` stops it.

Writing a value is the same `TOPIC_CBOR` shape in the other direction: topic id
of a `…/set` topic plus the CBOR value.

Example notification (single fragment):

```
81 00 04 01 00 18 18 26
│  └──┘ │  └──┘ └──┘
│   │   │   │    └── CBOR 0x1826 → 38
│   │   │   └── topic 0x0018 → power/battery/primary/info/soc_app
│   │   └── PayloadType 1 (TOPIC_CBOR)
│   └── remaining = 4
└── first fragment, seq_id 1
```

## 5. What you are allowed to see

The certificate's `r` (role) becomes the **broker username** `ble-role-N`, and
mosquitto's ACL decides what your subscription actually delivers — the nRF52's
certificate check is the real trust boundary, the broker just maps role → ACL.

| Role | Name | Access |
| --- | --- | --- |
| 7 | Owner | curated: `settings/#`, `update/#`, lock/unlock, `r ride/#`, `r power/state`, `r power/battery/primary/info/{soc,soc_app}`, `r eshifter/gear`, `r error/#`, `r ux/info/# ux/sensor/#`, **`r device/+/status`**, `ble/findmy/*`, `ftp_server` |
| 11 | Shared bike | as Owner, `settings` limited to a safe subset |
| 17 / 22 / 55 / 66 | Bike doctor / Bike hunter / QA / R&D | `readwrite #` (whole bus) |

So an ordinary **Owner** certificate is enough for the heartbeat + telemetry
view. Ids outside your ACL simply never produce notifications.

## 6. How this table was recovered

`spi-mqtt-bridge` builds a `std::map<uint16_t, std::string>` of the whole
registry at startup, in one function: **`FUN_001156e0`** (`0x1156e0`–`0x118f2b`,
Ghidra `/S5-v1.5/OS/spi-mqtt-bridge`, image base `0x100000`). Each entry is

```asm
mov   w2, #0x0030          ; the 16-bit topic id …
strh  w2, [sp, #0x50]      ; … stored into the key slot
bl    FUN_00119ac0         ; std::map::operator[](key)
add   x3, x3, #0x…         ; the topic string in .rodata
mov   x4, #0x11            ; its length
bl    FUN_00106210         ; std::string::assign
```

Ids were recovered by tracking those key stores against the string each insert
assigns. Note there are **two** key slots: most entries use `[sp, #0x50]`, but
where GCC inlined the red-black-tree lookup the key is staged in `[sp, #0x4e]`
via `w0` instead — missing that variant shifts the whole `update/*` block by one
entry, which is the trap in this function.

Result: **300 ids ⇄ 300 topic strings, complete and gapless** (every
topic-shaped string in `.rodata` is accounted for).

Cross-checks against the nRF52 image
(`ble.20240129.145222.1.5.0.main.v1.5.0-main.bin`, link base `0x23000`), which
carries the same ids for the topics it serves locally — broadcast table at
`0x67108` (14 × 12 bytes), dispatch table at `0x671e8` (2 × 12 bytes), entries
`{u16 id, u16 pad, u32 thumb_handler, u32 name_ptr}`:

| id | nRF52 handler name | bridge topic |
| --- | --- | --- |
| `0xf300` | `acknowledge_instance` | `ble/connection_state/acknowledge` |
| `0x00c0` | `version_info_req` | `ble/system/version_info` |
| `0x00c2` | `reset_reason_req` | `ble/system/reset_reason` |
| `0x00b0` | `ftp_command` | `ble/ftp/firmware/command` |
| `0x00e0` | `findmy_control` | `ble/findmy/control` |
| `0x00e3` | `findmy_serial_lookup` | `ble/findmy/serial_lookup` |
| `0x00e4` | `findmy_set_fmna_auth` | `ble/findmy/fmna_auth` |
| `0x00f1` | `settings_vm_public_key` | `ble/certificate_public_key` |
| `0x1020` | `modem_system_time` | `modem/system/time` |
| `0xbb00` / `0xbb01` / `0xbb02` | `findmy_set_serial_number` / `settings_vm_serial_number` / `settings_vm_bike_id` | `info/frame_number` / `info/ecu_serial` / `info/bike_id` |
| `0x00d0` | `update` | `ble/vars/update` |
| `0x0005` | `power_state_low_power` | `power/low_power` |
| `0x00c1` | `reboot` | `ble/system/reboot` |

Two OEM quirks worth knowing: `0xf100` (`ble/connections/handle`) is inserted
**twice** (identical key and value — harmless), and id `0x0066` is unused, so
`settings/assist_level` (`0x0065`) and `settings/assist_level/set` (`0x0067`) are
not adjacent.

## 7. Browser client sketch

Web Bluetooth works for all of this (Chrome desktop / Android — **not** iOS).

```js
const SVC    = 'e3d80000-3416-4a54-b011-68d41fdcbfcf';
const NOTIFY = 'e3d80001-3416-4a54-b011-68d41fdcbfcf';
const WRITE  = 'e3d80002-3416-4a54-b011-68d41fdcbfcf';

const dev  = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }] });
const gatt = await dev.gatt.connect();
const svc  = await gatt.getPrimaryService(SVC);
const rx   = await svc.getCharacteristic(NOTIFY);
const tx   = await svc.getCharacteristic(WRITE);

let seq = 1;
async function send(payloadType, body) {            // fragmenting writer
  const msg = Uint8Array.of(payloadType, ...body);
  const mtu = 20;                                   // conservative: 23-byte ATT MTU
  for (let off = 0, first = true; off < msg.length; first = false) {
    const chunk = msg.subarray(off, off + (mtu - 3));
    const hdr   = Uint8Array.of((first ? 0x80 : 0x00) | seq,
                                (msg.length - 1) >> 8, (msg.length - 1) & 0xff);
    await tx.writeValueWithoutResponse(Uint8Array.of(...hdr, ...chunk));
    off += chunk.length;
  }
  seq = seq === 127 ? 1 : seq + 1;
}

// after the certificate/challenge handshake: watch every ECU heartbeat
const ecus = [0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
              0x0038,0x0039,0x003a,0x003b,0x003c,0x003d,0x003e];
await rx.startNotifications();
await send(2, [0xE0, 0x01, ...ecus.flatMap(id => [id >> 8, id & 0xff])]);
```

Reassemble notifications per the fragment header, then decode
`[1][topic:2 BE][CBOR]` with any CBOR library.

## 8. The registry

### power / battery telemetry

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0001` | 1 | `power/state` |
| `0x0002` | 2 | `power/state/set` |
| `0x0003` | 3 | `power/battery/active` |
| `0x0004` | 4 | `power/state/status` |
| `0x0005` | 5 | `power/low_power` |
| `0x0006` | 6 | `power/low_power_extend` |
| `0x0007` | 7 | `power/deep_sleep` |
| `0x0008` | 8 | `power/state/extend_timeout` |
| `0x0009` | 9 | `power/battery/primary/info/power` |
| `0x0010` | 16 | `power/battery/primary/info/soc` |
| `0x0011` | 17 | `power/battery/primary/info/voltage` |
| `0x0012` | 18 | `power/battery/primary/info/charge_voltage` |
| `0x0013` | 19 | `power/battery/primary/info/charge_current` |
| `0x0014` | 20 | `power/battery/primary/info/discharge_current` |
| `0x0015` | 21 | `power/battery/primary/info/max_current` |
| `0x0016` | 22 | `power/battery/primary/info/health` |
| `0x0017` | 23 | `power/battery/primary/info/cycles` |
| `0x0018` | 24 | `power/battery/primary/info/soc_app` |
| `0x0019` | 25 | `power/battery/primary/info/temperature` |
| `0x0020` | 32 | `power/battery/lipo/info/soc` |
| `0x0021` | 33 | `power/battery/lipo/info/voltage` |
| `0x0022` | 34 | `power/battery/lipo/info/current_now` |
| `0x0023` | 35 | `power/battery/lipo/info/capacity` |
| `0x0024` | 36 | `power/battery/lipo/info/cycles` |
| `0x0025` | 37 | `power/battery/lipo/info/health` |
| `0x0026` | 38 | `power/battery/lipo/info/status` |
| `0x0027` | 39 | `power/battery/lipo/info/temp` |
| `0x0028` | 40 | `power/battery/lipo/info/pwr_avg` |
| `0x0029` | 41 | `power/battery/lipo/info/capacity_lvl` |
| `0x002a` | 42 | `power/battery/lipo/info/charge_full_design` |
| `0x002b` | 43 | `power/battery/lipo/info/charge_full` |

### per-ECU presence — **this is the CAN-heartbeat view**

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0030` | 48 | `device/ble/status` |
| `0x0031` | 49 | `device/battery_primary/status` |
| `0x0032` | 50 | `device/battery_secondary/status` |
| `0x0033` | 51 | `device/charger/status` |
| `0x0034` | 52 | `device/elock/status` |
| `0x0035` | 53 | `device/eshifter/status` |
| `0x0036` | 54 | `device/frontlight/status` |
| `0x0037` | 55 | `device/imx8_bridge/status` |
| `0x0038` | 56 | `device/modem/status` |
| `0x0039` | 57 | `device/motor_control/status` |
| `0x003a` | 58 | `device/motor_sensor/status` |
| `0x003b` | 59 | `device/power_control/status` |
| `0x003c` | 60 | `device/power_pedal/status` |
| `0x003d` | 61 | `device/rearlight/status` |
| `0x003e` | 62 | `device/user_ecu/status` |

### ride telemetry

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0049` | 73 | `ride/info/calories` |
| `0x0050` | 80 | `ride/info/speed` |
| `0x0051` | 81 | `ride/info/distance` |
| `0x0052` | 82 | `ride/info/pedal_rpm` |
| `0x0053` | 83 | `ride/info/motor_temperature` |
| `0x0054` | 84 | `ride/info/torque` |
| `0x0055` | 85 | `ride/info/motor_speed` |
| `0x0056` | 86 | `ride/info/motor_current` |
| `0x0057` | 87 | `ride/brake_level` |
| `0x0060` | 96 | `ride/boost` |

### settings

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0061` | 97 | `settings/region` |
| `0x0062` | 98 | `settings/region/set` |
| `0x0063` | 99 | `settings/mode` |
| `0x0064` | 100 | `settings/mode/set` |
| `0x0065` | 101 | `settings/assist_level` |
| `0x0067` | 103 | `settings/assist_level/set` |
| `0x0068` | 104 | `settings/shift_levels` |
| `0x0069` | 105 | `settings/shift_levels/set` |
| `0x006a` | 106 | `settings/light_mode` |
| `0x006b` | 107 | `settings/light_mode/set` |
| `0x006c` | 108 | `settings/light_auto_threshold` |
| `0x006d` | 109 | `settings/light_auto_threshold/set` |
| `0x006e` | 110 | `settings/alarm` |
| `0x006f` | 111 | `settings/alarm/set` |
| `0x0070` | 112 | `settings/animation_theme` |
| `0x0071` | 113 | `settings/animation_theme/set` |
| `0x0072` | 114 | `settings/bell_sound` |
| `0x0073` | 115 | `settings/bell_sound/set` |
| `0x0074` | 116 | `settings/backup_unlock_code` |
| `0x0075` | 117 | `settings/backup_unlock_code/set` |

### ux (sound, sensors, button, USB-C)

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0076` | 118 | `ux/sound/play` |
| `0x0077` | 119 | `ux/power` |
| `0x0078` | 120 | `ux/alarm/state` |
| `0x0079` | 121 | `settings/ride_animation_right` |
| `0x007a` | 122 | `settings/ride_animation_right/set` |
| `0x007b` | 123 | `settings/brake_lights` |
| `0x007c` | 124 | `settings/brake_lights/set` |
| `0x007d` | 125 | `settings/turning_lights` |
| `0x007e` | 126 | `settings/turning_lights/set` |
| `0x007f` | 127 | `ux/tracking/alarm/imu/triggered` |
| `0x0080` | 128 | `ux/button` |
| `0x0081` | 129 | `ux/sensor/light` |
| `0x0082` | 130 | `ux/sensor/humidity` |
| `0x0083` | 131 | `ux/sensor/temperature` |
| `0x0084` | 132 | `ux/sensor/air` |
| `0x0090` | 144 | `ux/usb/info/sink` |
| `0x0091` | 145 | `ux/usb/settings/enable` |
| `0x0092` | 146 | `ux/usb/settings/enable/set` |

### lock, trip distance, e-shifter

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x00a0` | 160 | `ux/lock/unlock` |
| `0x00a1` | 161 | `ux/lock/info/state` |
| `0x00a2` | 162 | `ux/lock/software_lock` |
| `0x00a3` | 163 | `ux/lock/touch_unlock_request` |
| `0x00a4` | 164 | `ux/lock/touch_unlock_activate` |
| `0x00a5` | 165 | `ux/info/distance` |
| `0x00a6` | 166 | `ux/lock/locking_while_riding` |
| `0x00a8` | 168 | `eshifter/gear` |
| `0x00a9` | 169 | `eshifter/gear/set` |
| `0x00aa` | 170 | `eshifter/state` |
| `0x00ab` | 171 | `eshifter/last_calibrated` |

### BLE module itself (FTP, system, Find My, NFC)

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x00b0` | 176 | `ble/ftp/firmware/command` |
| `0x00b1` | 177 | `ble/ftp/firmware/reply` |
| `0x00c0` | 192 | `ble/system/version_info` |
| `0x00c1` | 193 | `ble/system/reboot` |
| `0x00c2` | 194 | `ble/system/reset_reason` |
| `0x00d0` | 208 | `ble/vars/update` |
| `0x00e0` | 224 | `ble/findmy/control` |
| `0x00e1` | 225 | `ble/findmy/report` |
| `0x00e2` | 226 | `ble/findmy/sound` |
| `0x00e3` | 227 | `ble/findmy/serial_lookup` |
| `0x00e4` | 228 | `ble/findmy/fmna_auth` |
| `0x00e5` | 229 | `ble/findmy/provisioned` |
| `0x00e6` | 230 | `ble/findmy/event/pairing` |
| `0x00e7` | 231 | `ble/findmy/event/paired` |
| `0x00e8` | 232 | `ble/findmy/event/pairing_failed` |
| `0x00e9` | 233 | `ble/findmy/event/enable` |
| `0x00ea` | 234 | `ble/findmy/event/disable` |
| `0x00eb` | 235 | `ble/findmy/event/reset` |
| `0x00ec` | 236 | `ble/findmy/event/reset_ignored` |
| `0x00ed` | 237 | `ble/findmy/certified` |
| `0x00ef` | 239 | `ble/findmy/certify` |
| `0x00f0` | 240 | `ble/bike_resolving_key` |
| `0x00f1` | 241 | `ble/certificate_public_key` |
| `0x00f2` | 242 | `ble/heartbeat` |
| `0x00fa` | 250 | `ble/nfc/unlock_request` |

### tracking + per-service errors

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x0100` | 256 | `tracking/state` |
| `0x0200` | 512 | `service/power/error` |
| `0x0201` | 513 | `service/ux/error` |
| `0x0202` | 514 | `service/monitor/error` |
| `0x0203` | 515 | `service/update/error` |

### modem (nRF9160) — ctrl channel

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x1000` | 4096 | `modem/ftp/command` |
| `0x1001` | 4097 | `modem/ftp/reply` |
| `0x1005` | 4101 | `modem/at/command` |
| `0x1006` | 4102 | `modem/at/reply` |
| `0x1010` | 4112 | `modem/system/device_info` |
| `0x1020` | 4128 | `modem/system/time` |
| `0x10c0` | 4288 | `modem/system/version_info` |
| `0x10c1` | 4289 | `modem/system/reboot` |
| `0x10c2` | 4290 | `modem/system/reset_reason` |
| `0x10ca` | 4298 | `modem/nordic/version_info` |
| `0x10cb` | 4299 | `modem/nordic/update/config` |
| `0x10d0` | 4304 | `modem/vars/update` |
| `0x10d1` | 4305 | `modem/heartbeat` |

### FTP servers

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x2010` | 8208 | `ftp_server/command` |
| `0x2011` | 8209 | `ftp_server/reply` |
| `0x2012` | 8210 | `ftp_server/file_finished` |
| `0x3010` | 12304 | `phone/ftp_server/command` |
| `0x3011` | 12305 | `phone/ftp_server/reply` |

### logging

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x4001` | 16385 | `logging/event` |
| `0x4002` | 16386 | `logging/event/ble` |
| `0x4003` | 16387 | `logging/event/modem` |

### update / OTA

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x5001` | 20481 | `update/current_version` |
| `0x5010` | 20496 | `update/update_available` |
| `0x5011` | 20497 | `update/start` |
| `0x5012` | 20498 | `update/stage` |
| `0x5020` | 20512 | `update/progress` |
| `0x5030` | 20528 | `update/finished` |
| `0x5031` | 20529 | `update/ux/finished` |
| `0x5032` | 20530 | `update/stage2/device_update_started` |
| `0x5040` | 20544 | `update/user_button_pattern/roll_back` |
| `0x5050` | 20560 | `update/background_update/ble` |
| `0x5051` | 20561 | `update/background_update/battery_primary` |
| `0x5052` | 20562 | `update/background_update/battery_secondary` |
| `0x5053` | 20563 | `update/background_update/charger` |
| `0x5054` | 20564 | `update/background_update/elock` |
| `0x5055` | 20565 | `update/background_update/eshifter` |
| `0x5056` | 20566 | `update/background_update/frontlight` |
| `0x5057` | 20567 | `update/background_update/imx8_bridge` |
| `0x5058` | 20568 | `update/background_update/modem` |
| `0x5059` | 20569 | `update/background_update/motor_sensor` |
| `0x505a` | 20570 | `update/background_update/power_control` |
| `0x505b` | 20571 | `update/background_update/power_pedal` |
| `0x505c` | 20572 | `update/background_update/rearlight` |
| `0x505d` | 20573 | `update/background_update/motor_control` |
| `0x505e` | 20574 | `update/background_update/user_ecu` |
| `0x505f` | 20575 | `update/background_update/modem_nordic_stack` |
| `0x5060` | 20576 | `update/background_update/finished/ble` |
| `0x5061` | 20577 | `update/background_update/finished/battery_primary` |
| `0x5062` | 20578 | `update/background_update/finished/battery_secondary` |
| `0x5063` | 20579 | `update/background_update/finished/charger` |
| `0x5064` | 20580 | `update/background_update/finished/elock` |
| `0x5065` | 20581 | `update/background_update/finished/eshifter` |
| `0x5066` | 20582 | `update/background_update/finished/frontlight` |
| `0x5067` | 20583 | `update/background_update/finished/imx8_bridge` |
| `0x5068` | 20584 | `update/background_update/finished/modem` |
| `0x5069` | 20585 | `update/background_update/finished/motor_sensor` |
| `0x506a` | 20586 | `update/background_update/finished/power_control` |
| `0x506b` | 20587 | `update/background_update/finished/power_pedal` |
| `0x506c` | 20588 | `update/background_update/finished/rearlight` |
| `0x506d` | 20589 | `update/background_update/finished/motor_control` |
| `0x506e` | 20590 | `update/background_update/finished/user_ecu` |
| `0x506f` | 20591 | `update/background_update/finished/modem_nordic_stack` |
| `0x5070` | 20592 | `update/background_update/progress_info/ble` |
| `0x5071` | 20593 | `update/background_update/progress_info/battery_primary` |
| `0x5072` | 20594 | `update/background_update/progress_info/battery_secondary` |
| `0x5073` | 20595 | `update/background_update/progress_info/charger` |
| `0x5074` | 20596 | `update/background_update/progress_info/elock` |
| `0x5075` | 20597 | `update/background_update/progress_info/eshifter` |
| `0x5076` | 20598 | `update/background_update/progress_info/frontlight` |
| `0x5077` | 20599 | `update/background_update/progress_info/imx8_bridge` |
| `0x5078` | 20600 | `update/background_update/progress_info/modem` |
| `0x5079` | 20601 | `update/background_update/progress_info/motor_sensor` |
| `0x507a` | 20602 | `update/background_update/progress_info/power_control` |
| `0x507b` | 20603 | `update/background_update/progress_info/power_pedal` |
| `0x507c` | 20604 | `update/background_update/progress_info/rearlight` |
| `0x507d` | 20605 | `update/background_update/progress_info/motor_control` |
| `0x507e` | 20606 | `update/background_update/progress_info/user_ecu` |
| `0x507f` | 20607 | `update/background_update/progress_info/modem_nordic_stack` |

### per-ECU firmware / bootloader / vendor versions

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x5130` | 20784 | `device/ble/version/firmware` |
| `0x5131` | 20785 | `device/battery_primary/version/firmware` |
| `0x5132` | 20786 | `device/battery_secondary/version/firmware` |
| `0x5133` | 20787 | `device/charger/version/firmware` |
| `0x5134` | 20788 | `device/elock/version/firmware` |
| `0x5135` | 20789 | `device/eshifter/version/firmware` |
| `0x5136` | 20790 | `device/frontlight/version/firmware` |
| `0x5137` | 20791 | `device/imx8_bridge/version/firmware` |
| `0x5138` | 20792 | `device/modem/version/firmware` |
| `0x5139` | 20793 | `device/motor_control/version/firmware` |
| `0x513a` | 20794 | `device/motor_sensor/version/firmware` |
| `0x513b` | 20795 | `device/power_control/version/firmware` |
| `0x513c` | 20796 | `device/power_pedal/version/firmware` |
| `0x513d` | 20797 | `device/rearlight/version/firmware` |
| `0x513e` | 20798 | `device/user_ecu/version/firmware` |
| `0x5230` | 21040 | `device/ble/version/bootloader` |
| `0x5231` | 21041 | `device/battery_primary/version/bootloader` |
| `0x5232` | 21042 | `device/battery_secondary/version/bootloader` |
| `0x5233` | 21043 | `device/charger/version/bootloader` |
| `0x5234` | 21044 | `device/elock/version/bootloader` |
| `0x5235` | 21045 | `device/eshifter/version/bootloader` |
| `0x5236` | 21046 | `device/frontlight/version/bootloader` |
| `0x5237` | 21047 | `device/imx8_bridge/version/bootloader` |
| `0x5238` | 21048 | `device/modem/version/bootloader` |
| `0x5239` | 21049 | `device/motor_control/version/bootloader` |
| `0x523a` | 21050 | `device/motor_sensor/version/bootloader` |
| `0x523b` | 21051 | `device/power_control/version/bootloader` |
| `0x523c` | 21052 | `device/power_pedal/version/bootloader` |
| `0x523d` | 21053 | `device/rearlight/version/bootloader` |
| `0x523e` | 21054 | `device/user_ecu/version/bootloader` |
| `0x5330` | 21296 | `device/ble/version/vendor` |
| `0x5331` | 21297 | `device/battery_primary/version/vendor` |
| `0x5332` | 21298 | `device/battery_secondary/version/vendor` |
| `0x5333` | 21299 | `device/charger/version/vendor` |
| `0x5334` | 21300 | `device/elock/version/vendor` |
| `0x5335` | 21301 | `device/eshifter/version/vendor` |
| `0x5336` | 21302 | `device/frontlight/version/vendor` |
| `0x5337` | 21303 | `device/imx8_bridge/version/vendor` |
| `0x5338` | 21304 | `device/modem/version/vendor` |
| `0x5339` | 21305 | `device/motor_control/version/vendor` |
| `0x533a` | 21306 | `device/motor_sensor/version/vendor` |
| `0x533b` | 21307 | `device/power_control/version/vendor` |
| `0x533c` | 21308 | `device/power_pedal/version/vendor` |
| `0x533d` | 21309 | `device/rearlight/version/vendor` |
| `0x533e` | 21310 | `device/user_ecu/version/vendor` |

### per-ECU serials, charger, monitor

| id | dec | MQTT topic |
| --- | --- | --- |
| `0x6000` | 24576 | `device/ble/serial` |
| `0x6001` | 24577 | `device/battery_primary/serial` |
| `0x6002` | 24578 | `device/battery_secondary/serial` |
| `0x6003` | 24579 | `device/charger/serial` |
| `0x6004` | 24580 | `device/elock/serial` |
| `0x6005` | 24581 | `device/eshifter/serial` |
| `0x6006` | 24582 | `device/frontlight/serial` |
| `0x6007` | 24583 | `device/imx8_bridge/serial` |
| `0x6008` | 24584 | `device/modem/serial` |
| `0x6009` | 24585 | `device/motor_control/serial` |
| `0x600a` | 24586 | `device/motor_sensor/serial` |
| `0x600b` | 24587 | `device/power_control/serial` |
| `0x600c` | 24588 | `device/power_pedal/serial` |
| `0x600d` | 24589 | `device/rearlight/serial` |
| `0x600e` | 24590 | `device/user_ecu/serial` |
| `0x6100` | 24832 | `device/charger/connected` |
| `0x6101` | 24833 | `device/charger/mode` |
| `0x6102` | 24834 | `device/charger/current` |
| `0x6103` | 24835 | `device/charger/voltage` |
| `0x6104` | 24836 | `device/charger/finished` |
| `0x7300` | 29440 | `monitor/component/charger/type` |

### bike identity

| id | dec | MQTT topic |
| --- | --- | --- |
| `0xbb00` | 47872 | `info/frame_number` |
| `0xbb01` | 47873 | `info/ecu_serial` |
| `0xbb02` | 47874 | `info/bike_id` |
| `0xbb03` | 47875 | `info/sku` |

### maintenance + bridge control

| id | dec | MQTT topic |
| --- | --- | --- |
| `0xe000` | 57344 | `maintenance/battery/primary/reset` |
| `0xe001` | 57345 | `bridge/subscribe` |
| `0xe002` | 57346 | `bridge/unsubscribe` |

### BLE link control (bridge-internal)

| id | dec | MQTT topic |
| --- | --- | --- |
| `0xf000` | 61440 | `ble/connections/config` |
| `0xf00f` | 61455 | `ble/proxy` |
| `0xf010` | 61456 | `ble/proxy/config` |
| `0xf100` | 61696 | `ble/connections/handle` |
| `0xf200` | 61952 | `ble/connections/info` |
| `0xf300` | 62208 | `ble/connection_state/acknowledge` |

### modem info (bridge-internal)

| id | dec | MQTT topic |
| --- | --- | --- |
| `0xfa00` | 64000 | `modem/config/lte` |
| `0xfa01` | 64001 | `modem/info/device` |
| `0xfa02` | 64002 | `modem/info/network` |
| `0xfa03` | 64003 | `modem/info/datetime` |
| `0xfa04` | 64004 | `modem/tracking/type/set` |
| `0xfa05` | 64005 | `modem/sms` |
| `0xfa06` | 64006 | `modem/location/cellular` |
| `0xfa07` | 64007 | `modem/location/gnss` |
