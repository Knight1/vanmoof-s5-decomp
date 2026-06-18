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

- **SoC:** NXP **i.MX8M Nano** — quad **Cortex-A53** @ ARMv8-A, **LPDDR4**.
  Built `-mcpu=cortex-a53+crc+crypto`. DTB model: *"NXP VanMoof mainECU
  i.MX8MNano board"*. CAAM hardware crypto + SNVS secure storage.
- **SPI satellites:** a VanMoof kernel driver (`vm,mainecu_spi`) attaches four
  chips over SPI — **nRF52840** (BLE), **nRF9160** (modem), **NXP SR150 UWB**
  (secure ranging / phone-as-key), and an **LPC55Sxx** secure MCU (key store /
  immobilizer). BLE+modem are bridged to MQTT; UWB+LPC55 go through kernel/other
  paths. See [`hardware.md`](hardware.md).
- **CAN bridge:** **`imx8_bridge`** (shipped in `/opt/devices_fw`) is a
  **discrete Cortex-M MCU** running FreeRTOS — SPI-slave to the i.MX8, CAN node
  to the mechatronics fleet. It is flashed as a separate SPI device by `update`,
  so it is *not* the SoC's own Cortex-M7 (likely the `lpc55sxx` SPI satellite —
  see [`../../imx8_bridge/`](../../imx8_bridge/)).
- **`jailhouse.ko`** (Siemens partitioning hypervisor, v0.12) + its
  `jailhouse.bin` firmware are present, but no cell configs ship in the rootfs,
  so what runs in a partition here is unconfirmed — see
  [`kernel-modules.md`](kernel-modules.md).
- **Storage:** eMMC `mmcblk2`, **A/B dual-slot** (confirmed from `runFOTA.sh`):
  bootloader in `boot0`/`boot1`, kernel in `p2`/`p3`, rootfs in `p4`/`p5`,
  shared `p6` = ext4 config/persistent, `p1` = vfat (misc). U-Boot env raw at
  `mmcblk2 @ 0x400000`. Slot switch via `mmc bootpart enable`, with a
  verify-or-rollback safe-update state in U-Boot `su_state`. Full map +
  Pegatron image format in [`fota-image.md`](fota-image.md).
- **Root fs:** read-only **SquashFS** (gzip). Writable state is confined to
  tmpfs (`/run`, `/var/volatile`) and the ext4 config partition via bind mounts.
- **Power HW:** internal **LiPo** battery (TI bq27542 gauge + BQ25672 charger on
  I²C bus 2) + the removable **primary** Panasonic pack on CAN; RTC + IMU
  wake sources. See [`hardware.md`](hardware.md).

## The MQTT bus (IPC)

A loopback-only `mosquitto 1.6.7` instance (`listener 1883`, `bind_interface
lo`) is the inter-process bus. Design points:

- **Internal-only:** bound to `lo`, never exposed off-box. Reachability off the
  bike is exclusively through `gateway` (cloud) and the BLE proxy.
- **ACL'd, but not authenticated:** there is no password file — the broker is
  anonymous and enforces `/etc/mosquitto/acl` by the *claimed* username. Service
  users (`power-service`, `ux-service`, `gateway`, …) and **BLE rider roles**
  (`ble-role-7` = Owner, `ble-role-11` = Shared bike, …) each get a topic
  allow-list; the BLE cert auth happens on the nRF52, not the broker. Full map
  in [`mqtt-bus.md`](mqtt-bus.md).
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
