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
