# Architecture — the main module

The main module is the only general-purpose computer on the bike. It runs
Linux on an i.MX8M Nano and acts as the hub between three worlds:

1. the **rider's phone / cloud** — over BLE (nRF52) and cellular (nRF9160);
2. the **bike's mechatronics** — the CAN fleet of Cortex-M sub-ECUs;
3. **VanMoof's backend** — AWS IoT, for telemetry, remote jobs, and Find-My.

Everything is glued together by a single **MQTT broker on loopback**. Each
subsystem is an MQTT client; bridges adapt the non-IP links (SPI, CAN) onto
the bus.

```
                         ┌──────────────────────── AWS IoT ────────────────────────┐
                         │   Device Shadow · Jobs · telemetry rules · Find-My        │
                         └──────────────▲───────────────────────────────────────────┘
                                        │ MQTT/TLS over cellular PPP
                                 ┌──────┴───────┐
                                 │   gateway    │  (Go)  cloud ⇄ bus bridge
                                 └──────┬───────┘
   nRF52 BLE ──SPI──▶ spi-mqtt-bridge@ble ─┐        ┌─ tracking  (theft/alarm)
   nRF9160  ──SPI──▶ spi-mqtt-bridge@modem ┤        ├─ monitor   (health)
                                            │        ├─ ux        (lock/unlock/UI)
                                   ┌────────▼────────▼───┐
                                   │   mosquitto broker   │   loopback MQTT bus
                                   │   (lo:1883, ACL)     │   the IPC backbone
                                   └────────▲────────▲────┘
                                            │        │
   CAN fleet ─SPI─▶ imx8_bridge ─▶ spi-can-if-linux ─┤        ├─ power  (battery/charger)
   (user_ecu, motor,                                 │        ├─ ride   (pedal assist)
    elock, lights, …)                                │        ├─ update (peripheral OTA)
                                                      └─ logging / mqtt-ftp / …
```

## SoC & boot (hardware)

- **SoC:** NXP **i.MX8M Nano** — quad **Cortex-A53** @ ARMv8-A. Built with
  `-mcpu=cortex-a53+crc+crypto`. `pinctrl_imx8mn` is the built-in pinctrl.
- **Co-processor:** the SoC's Cortex-M core runs **`imx8_bridge`** (shipped in
  `/opt/devices_fw`), the SPI↔CAN gateway to the mechatronics bus. The
  **`jailhouse.ko`** hypervisor partitions the SoC so the M-core / RT workload
  is isolated from Linux.
- **Storage:** eMMC `mmcblk2`. U-Boot env at `mmcblk2 @ 0x400000`
  (`/etc/fw_env.config`). Partition map in `fota-image.md`:
  - `mmcblk2p1` — vfat (boot: U-Boot / kernel / dtb)
  - `mmcblk2p2` / `p3` — **A/B** root slots (the SquashFS this image flashes to)
  - `mmcblk2p6` — ext4 **config / persistent** (logs, timesync, mosquitto
    persistence, provisioning)
- **Root fs:** read-only **SquashFS** (gzip). Writable state is confined to
  tmpfs (`/run`, `/var/volatile`) and the ext4 config partition via bind mounts.

## The MQTT bus (IPC)

A loopback-only `mosquitto 1.6.7` instance (`listener 1883`, `bind_interface
lo`) is the inter-process bus. Design points:

- **Internal-only:** bound to `lo`, never exposed off-box. Reachability off the
  bike is exclusively through `gateway` (cloud) and the BLE proxy.
- **Authenticated & ACL'd:** every client authenticates as a named user and is
  constrained by `/etc/mosquitto/acl`. Service users (`power-service`,
  `ux-service`, `gateway`, …) and **BLE rider roles** (`ble-role-7` = Owner,
  `ble-role-11` = Shared bike, …) each get a topic allow-list. Full map in
  `mqtt-bus.md`.
- **Persistent:** retained messages survive reboot via
  `mmcblk2p6/config/mosquitto/`.

Topic namespace (high level): `power/…`, `ride/…`, `ux/lock/…`, `ux/info/…`,
`ux/sensor/…`, `settings/…`, `update/…`, `ble/…`, `eshifter/…`, `error/…`,
`device/+/status`, `ftp_server/…`.

## Data flows

**Rider unlocks (BLE):** phone → nRF52 → `spi-mqtt-bridge@ble` → bus
(`ux/lock/unlock`, gated by the rider's `ble-role-N` ACL) → `ux` runs the
unlock sequence, commands `elock` via the CAN side (`update`/`power`/CAN
bridge), publishes `ux/lock/info/state`.

**Pedal assist (ride):** `ride` subscribes to pedal-sensor / motor / power
topics fed up from the CAN fleet through `imx8_bridge` →
`spi-can-if-linux`, applies its `RideStrategy`, and commands the motor
controller back down the same path.

**Telemetry / remote jobs (cloud):** services publish telemetry to the bus →
`gateway` batches and forwards to **AWS IoT** (`$aws/rules/telemetry/…`),
mirrors device state to the **Device Shadow** (`$aws/things/<thing>/shadow`),
and executes backend-pushed **Jobs** (`$aws/things/<thing>/jobs/…`) — e.g.
firmware update commands handed to `update`.

**OTA update:** `update /opt/devices_fw` walks `manifest.txt` and flashes each
sub-ECU image over the CAN/SPI path (and the modem via `vmxs5-modem-update`),
honoring `AllowSkip` / `DontRollback`. The Linux rootfs itself updates via the
A/B SquashFS slots and `vmxs5-upgrade-scripts`.

## Process supervision

All VanMoof services are systemd units with `Restart=on-failure`,
`RestartSec=5s`, `StartLimitBurst=5`. Ordering is expressed via `After=` on
`mosquitto.service` and the bridge services (`power` waits for the CAN + BLE +
modem bridges; `ux` waits for `power` and `update`; `ride` waits for `power`).
`power` and `update` use `Type=notify` (sd_notify readiness).

See `services.md` for the per-unit detail and `mqtt-bus.md` for the bus map.
