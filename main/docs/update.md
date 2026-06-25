# Update subsystem

The bike has **two** distinct update mechanisms, both orchestrated from the main
module:

1. **i.MX8 self-update** — the main module's own bootloader/kernel/rootfs, via
   the Pegatron A/B installer `runFOTA.sh` (see [`fota-image.md`](fota-image.md)).
2. **Peripheral update** — every other ECU (BLE, modem, motor, lights, lock,
   power boards, …), via the `/usr/bin/update` service over the CAN/SMP buses.

Cloud orchestration sits on top: an **AWS IoT Job** (handled by `gateway`)
delivers the FOTA bundle and triggers the flow; `update` reports progress back
on the MQTT bus, which `gateway` mirrors to the Device Shadow.

## `update` service (peripheral OTA)

`ExecStart=/usr/bin/update /opt/devices_fw` (`Type=notify`). Built from the
embedded monorepo under `devices/main/update/src/`:

| Source unit | Role |
| --- | --- |
| `update_service` / `main` | top-level orchestration, systemd `notify` |
| `manifest` | parses `manifest.txt` (`/opt/devices_fw` or `/tmp/root.sqfs/opt/devices_fw`) |
| `version_client_mqtt` | queries each device's running version over MQTT |
| `background_update` | the multi-stage update driver (`UpdateStage`) |
| `thirdparty_update_client` | generic per-page CRC flashing client (motors, lights, lock, …) |
| `lightweight_update_client` | trimmed flashing client variant |
| `mqtt_update_client` | update transport over the MQTT bus |
| `smp_modem_update_client` | **SMP/MCUmgr** client for the Nordic parts (modem/BLE) |
| `runfota` | shells out to `runFOTA.sh` for the i.MX8 self-update |

CLI: `-b <bus>` (CAN bus, `vcan0` default), `-f/--force` (update even if the
version already matches), `-V` (build version).

### What it does

- Reads `manifest.txt` → for each device, compares the manifest version against
  the device's reported version (skips if equal unless `--force`; honors
  `AllowSkip` / `DontRollback`).
- Drives a **page-based transfer with CRC**: per-page CRC (retried up to 3×:
  *"CRC check of a page failed 3 times"*) plus a full-image CRC
  (*"CRC over full image"*, *"CRC check of the entire image failed"*).
- Targets carry their **own A/B boot control**: *"Boot control version: %s,
  partition: %d, stage: %d"* — i.e. the sub-ECUs do the same flash-inactive /
  verify / commit dance the i.MX8 does, just at MCU scale.
- **Two-stage** with rollback: *"Failed to install devices in update 'Stage 2',
  rolling back"*, *"Failed to set execute reboot after update 'Stage 1'"* — stage 1
  loads/prepares all devices, stage 2 commits + reboots; failure rolls the set
  back.
- Uses `common::RetryableAction` (bounded retries with timeout) for each
  request/response; *"Device refused to apply update"*, *"Devices have not all
  come back after update"* are the failure paths.

### Supplier dispatch (Panasonic vs DynaPack vs LiteON)

Each manifest entry is turned into a concrete flashing client by
`UpdateClientFactory::GetUpdateClient(name)` (decompiled at `update` `0x123220`),
which matches the device/file-name **token**:

| Name token | Client class | Target |
| --- | --- | --- |
| `…_panasonic` | `PanasonicUpdateClient` | primary battery (CAN node `0xA4`) |
| `…_dynapack` | `DynapackUpdateClient` | primary battery (CAN node `0xA4`) |
| `charger…` | `LiteonUpdateClient` | external charger (CAN node `0xA7`) |
| modem/motor tokens | `SmpModemUpdateClient` / `C2000UpdateClient` | nRF modem / TI motor |
| `battery_primary_*` with neither supplier | — (throws) | *"first token of file name should be battery_primary_panasonic or battery_primary_dynapack"* |

So the **battery supplier is never sensed at runtime** — it is fixed by the FOTA
package file name (`battery_primary_panasonic` / `battery_primary_dynapack`).
Panasonic and DynaPack are *different* `ThirdPartyUpdateClient` subclasses with
different page/timeout constants; Panasonic additionally has a **version gate**
(firmware > v1.3.0.255 flashes ~3× faster). Full decomp detail:
[`../update/README.md`](../update/README.md). This is what `power`'s static
`battery_primary_{panasonic,dynapack}` name registry feeds into.

### Page-CRC-over-CAN flash protocol (decompiled)

`PanasonicUpdateClient`, `DynapackUpdateClient` and `LiteonUpdateClient` all
derive from **`ThirdPartyUpdateClient`** (ctor `0x139660`); only the subclass
ctor differs (CAN node + timing/limit constants + vtable). The shared base does
the actual flashing. All three were decompiled from `update`; the recovered
functions are named in the Ghidra DB (`thirdparty_*`, `od_send_*`).

**Per-client ctor constants** (object layout: `obj[9..0xb]` = ms timeouts,
`obj[0xc]` = current/charge limit, `obj[0xd]` = mode flag):

| Client | ctor | node a0 | timeouts (ms) | limit | mode | notes |
| --- | --- | --- | --- | --- | --- | --- |
| Panasonic | `0x13b6a0` | `0xA4` | 100k/10k/10k (fw>1.3.0.255) **or** 300k/30k/30k | 360e6 / 900e6 | 1 | version-gated; vtable `0x19d1b8` |
| Dynapack | `0x13b620` | `0xA4` | 50k/1k/0 | — | 0 | vtable `0x19d180` |
| Liteon (charger) | `0x13b750` | `0xA7` | 20k/10k/10k | — | 0 | also subscribes OD `charger_status`; vtable `0x19d1f0` |

**OD operation map** — every flash op is an OD signal on the target node (a0),
group `0x8A`, with the op selected by a sub-index (high byte of the OD lookup
key). Senders are wired in the base ctor (names `flash`/`ErasePage`/`write`/`crc`
read from the binary's literal pool):

| Sub | Op | Sender | DLC | Payload / reply |
| --- | --- | --- | --- | --- |
| `0x81` | flash (size-info request) | `od_send_flash_0x81` (`0x13d940`) | 4 | reply = `page_size`(BE16) ‖ `page_count`(BE16) |
| `0x82` | ErasePage | `od_send_erasepage_0x82` (`0x13d9f0`) | 4 | `{ swap16(pageIdx), 0x0100 }` |
| `0x83` | write (page data) | `od_send_write_0x83` (`0x13daa0`) | 8 | one 8-byte image chunk |
| `0x84` | page CRC query | `od_send_pagecrc_0x84` (`0x13db50`) | 4 | 2-byte CRC reply |
| `0x85` | image CRC / apply | `od_send_imagecrc_0x85` (`0x13dc20`) | 1 | 4-byte CRC reply |
| `0x90` | status / reboot | `od_send_status_reboot_0x90` (`0x13def0`) | 1 | `od_reboot_charger` (`0x13e090`) fires the reboot pulse |

The OD layer (libvm) maps `{a0,a1,a2,a3}` to the 29-bit CAN id via the verified
encoder `vm_can_tx` (`update` `0x16c5a0` / `power` `0x158b60`):
`id = (a0<<21) | (a1<<13) | (a2<<5) | (a3&0x1F)`, EFF flag on the Linux side. The
flash senders write `a0=node` and a packed key to the descriptor: the key's low
byte → `a1` (the op sub-index `0x81..0x90`), `0x8A` → `a2` (the FOTA group), key
high byte → the TP *kind* (0=single-frame data, 2=multiframe) — **not** the id.
`a3` is dynamic: single-frame data ops (`0x81/0x84/0x85`) OR `0x10` (request bit);
multiframe ops (`0x82/0x83/0x90`) start `a3=0` and increment the TP sequence.

**Derived charger flash command IDs** (`a0=0xA7`, `a2=0x8A`):

| Op | a1 | wire id (i.MX8 → charger) |
| --- | --- | --- |
| flash / size-info | `0x81` | `0x14F03150` (a3=0x10) |
| ErasePage | `0x82` | `0x14F05140` (+seq) |
| write | `0x83` | `0x14F07140` (+seq) |
| page CRC | `0x84` | `0x14F09150` (a3=0x10) |
| image CRC | `0x85` | `0x14F0B150` (a3=0x10) |
| reboot | `0x90` | `0x14F21140` |

These are the **command** ids (high confidence, from the sender keys + the
encoder). The charger's **reply** ids (size-info / CRC results that drive the FSM)
are emitted by the charger's **off-image bootloader** — its application image has
no FOTA receiver (HW filter accepts all `a0=0xA7`; flash-receive + RX-IRQ live
past the `0x5b2c` image end). So the reply ids + exact payloads still need a live
candump of a real charger update to confirm. (Battery flash is the same protocol
at `a0=0xA4`. The separate `a1=0x8A` *version-request* path — `power` `0x137638`,
`{a0=0xA3,a1=0x8A,a2=0x80}` — is the pre-flash version query, gated on node
`0xA7`/`0xA4`, not a flash data op.)

**Flash FSM** (`ThirdPartyUpdateClient`, state word @ `obj+0xf4`):

```
StartUpdate (0x138940)  ── VM-call "Initial flash size request" (3s) ──▶ state 2
   on size-info reply  thirdparty_on_flash_size_info (0x1383e0):
       page_size  = swap16(reply & 0xFFFF)        @ obj+0x120
       page_count = swap16(reply >> 16)           @ obj+0x128
       require (image_len + 4) <= page_size*page_count   ── else abort ──▶ TERMINAL
   ──▶ state 3 (PAGE_CRC)
 per page  AdvancePage (0x1382c0, VM-call "AdvancePage" 5s):
       thirdparty_write_page (0x137580): alloc page_size, memset 0xFF pad,
           memcpy image slice, CRC32 ─▶ expected @ obj+0x134
           ErasePage(0x82) ; stream page 8 bytes/frame via write(0x83) ;
           query page CRC(0x84)
       thirdparty_on_crc_response (0x139000):
           match  & last page ─▶ state 4 + whole-image CRC32 + VM-call
                                  "CRC over full image" (5s)
           match  & more      ─▶ next AdvancePage
           mismatch           ─▶ retry; 3rd failure ─▶ abort
                                  ("CRC check of a page failed 3 times")
   state 4 (IMAGE_CRC):
       match    ─▶ state 5 + VM-call "Update request" (10s) = reboot/commit
       mismatch ─▶ abort ("CRC check of the entire image failed")
   ──▶ state 7 (TERMINAL): thirdparty_set_update_result (0x137790), notify_all
```

- **Page size / count are negotiated, not fixed** — the target reports them
  big-endian in the `0x81` reply. Pages are padded to `page_size` with `0xFF`
  before CRC and streamed 8 bytes per `0x83` frame.
- **CRC** is standard CRC-32 (`crc32_ieee` `0x16ee70`: reflected poly
  `0xEDB88320`, init `0xFFFFFFFF`, final one's-complement) for both per-page and
  whole-image; the device returns its CRC byte-swapped.
- **Retry count = 3** per page; on the 3rd page-CRC mismatch the whole update
  aborts to TERMINAL.
- Liteon-only: `thirdparty_validate_charger_type` (`0x138d80`) checks the charger
  type against its max current before `StartUpdate`.

The transport is `thirdparty_post_vm_call` (`0x137fb0`): an async libvm call by
name with completion + timeout(arg×1000 ms) callbacks; the named calls map onto
the OD commits above.

### MQTT topics

| Topic | Direction | Use |
| --- | --- | --- |
| `device/+/status` | in | per-device presence/status |
| `device/+/version/firmware/#` | in | running firmware version |
| `device/+/version/bootloader/#` | in | bootloader version |
| `device/+/version/vendor/#` | in | vendor/3rd-party version |
| `update/start` | in | trigger (writable by `gateway` ⇐ cloud Job) |
| `update/#` | out | progress / result |
| `ble/system/reboot`, `modem/system/reboot` | out | reboot a Nordic device after flashing |
| `modem/nordic/update/config`, `modem/nordic/version_info` | in/out | modem (nRF9160) update handshake |
| `modem/config/lte`, `modem/ftp/command`, `modem/ftp/reply` | — | modem config / file transfer |

### Nordic (modem & BLE) updates

The nRF9160 modem and nRF52 BLE parts are updated over **SMP (MCUmgr)** rather
than the page-CRC CAN protocol. The modem also ships a vendor baseband image
`/opt/devices_fw/mfw_nrf9160_1.3.1.zip`, flashed via `vmxs5-modem-update`
(`/usr/lib/python3.7/update_modem.py` using **`pynrfjprog`**). After a Nordic
update the service clears the retained `modem/nordic/version_info` message.

## i.MX8 self-update (`runFOTA.sh`)

The `runfota` client invokes `/usr/bin/runFOTA.sh` (script v1.3.11) on the
delivered `VM-XS5_FOTA`. That script implements the A/B ping-pong, MD5
verification, safe-update state machine (U-Boot `su_state`) and optional
`xdelta3` delta patching — all detailed in [`fota-image.md`](fota-image.md).

## End-to-end OTA flow (production)

```
AWS IoT Job ─▶ gateway ─▶ download VM-XS5_FOTA to /tmp/download
   │                         │
   │                         ├─ peripherals: update service walks manifest.txt,
   │                         │   flashes each ECU (CAN page+CRC / SMP), 2-stage + rollback
   │                         └─ i.MX8 self: runFOTA.sh writes inactive slot,
   │                             flips boot partition, verify-or-rollback on reboot
   └─ progress on update/#  ─▶ gateway ─▶ Device Shadow (cloud sees status)
```

## Helper scripts (`vmxs5-utils`)

- `runFOTA.sh` — the A/B installer (above).
- `shipping_mode.sh` — puts the bike to sleep for shipping: waits for VBUS/VAC
  absent, sets the **BQ25672** charger into ship mode (I²C reg `0x11`/`0x14`),
  hibernates the **bq27542** gauge, then `poweroff`. See [`hardware.md`](hardware.md).
- `gpio_func.sh` — sysfs GPIO export/get/set helper (group×32+pin numbering).
- `backup_machine_id.sh` — mirrors `/etc/machine-id` to the persistent config
  partition so identity survives a rootfs swap.
