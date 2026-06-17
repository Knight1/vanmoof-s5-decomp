# What VanMoof changed vs the OEM NXP image

The "OEM version" here is the **NXP i.MX Release Distro 5.4-zeus** reference
image (a generic Yocto/OpenEmbedded rootfs NXP publishes for its i.MX8 eval
boards). VanMoof took that image essentially as-is and layered a private Yocto
layer — call it `meta-vmxs5` — on top. **Almost nothing in the base OS was
modified; everything VanMoof did is *additive*.**

The evidence is the dpkg database (`/var/lib/dpkg/status`): **413 packages**
installed, of which the VanMoof-authored ones are the ~23 `vmxs5-*` recipes
below. The other ~390 are stock zeus (systemd, glibc, busybox, mosquitto,
openssl, kernel modules, wifi firmware, …).

## The VanMoof layer — `vmxs5-*` packages

| Package | Installs | What it is |
| --- | --- | --- |
| `vmxs5-gateway` | `/usr/bin/gateway` | **Cloud bridge** — Go service, AWS IoT (Shadow/Jobs/telemetry) ↔ MQTT bus. The only Go binary. |
| `vmxs5-embedded-power` | `/usr/bin/power` | **Battery / charger state machine** (C++). Power sequencing, charging, shipping/sleep states. |
| `vmxs5-embedded-ride` | `/usr/bin/ride` | **Pedal-assist ride controller** (C++) — `RideService(IMotor, IPower, IPedalSensor, RideStrategy)`. |
| `vmxs5-embedded-ux` | `/usr/bin/ux` | **User-experience / lock service** (C++) — lock/unlock, touch-unlock, `BackupUnlock` (kick-code), sounds/animations. |
| `vmxs5-embedded-tracking` | `/usr/bin/tracking` | **Anti-theft / tracking** (C++) — alarm state machine, location/theft reporting. |
| `vmxs5-embedded-monitor` | `/usr/bin/monitor` | **System health monitor** (C++) — watches components (modem, etc.), error-state reporting. |
| `vmxs5-embedded-update` | `/usr/bin/update` | **Peripheral OTA updater** (C++) — flashes `/opt/devices_fw/*` to the sub-ECUs. `Type=notify`. |
| `vmxs5-embedded-lightweight-update` | (update helper) | trimmed update path. |
| `vmxs5-embedded-motor-update-lib` | (lib) | motor-controller update routines. |
| `vmxs5-modem-update` | (modem fw flasher) | nRF9160 modem firmware update (`mfw_nrf9160_1.3.1.zip`). |
| `vmxs5-embedded-logging` | `/usr/bin/logging` | **Logging service** (C++) — collects logs off the bus to `/var/log` (eMMC-backed). |
| `vmxs5-embedded-mqtt-ftp` | `/usr/bin/mqtt-ftp-service` | **File transfer over MQTT** — pushes/pulls files via the bus (log/cert/config transport). |
| `vmxs5-embedded-spi-mqtt-bridge` | `/usr/bin/spi-mqtt-bridge` | **SPI ↔ MQTT** bridge, instantiated for `ble` and `modem` — puts the BLE SoC and the modem onto the bus. |
| `vmxs5-embedded-spi-can-bridge` | `/usr/bin/spi-can-if-linux` | **SPI ↔ CAN ↔ SPI** bridge to the `imx8_bridge` co-processor — puts the CAN fleet onto the bus. |
| `vmxs5-vcan-starter` | `/usr/sbin/start_vcan.sh` | brings up the `vcan0` virtual CAN interface at boot. |
| `vmxs5-input-event` | (input handling) | handlebar / button input event plumbing. |
| `vmxs5-device-binaries` | `/opt/devices_fw/*` | **the sub-ECU firmware bundle** — every peripheral image (`ble`, `user_ecu`, `elock`, motors, lights, …) + `manifest.txt`, staged for `update`. |
| `vmxs5-embedded-mosquitto-conf` | `/etc/mosquitto/*` | the broker config + the **role-based ACL** (`mqtt-bus.md`). |
| `vmxs5-embedded-ppp-conf` | `/etc/ppp/*`, chatscripts | the **cellular PPP** dial config for the nRF9160 modem. |
| `vmxs5-upgrade-scripts` | upgrade hooks | A/B image swap / post-install scripts. |
| `vmxs5-utils` | misc `/usr/bin` | VanMoof CLI utilities. |
| `vmxs5-version` | version metadata | stamps `/etc/firmware_version`, `/etc/firmware_imagetype`. |

(See `services.md` for what each binary actually does and how they wire up.)

## Other additive changes on top of stock zeus

- **Identity:** hostname `vmxs5mainecu`; `/etc/firmware_version=v1.5.0-main`,
  `/etc/firmware_imagetype=production`. A `root` login exists with a
  `$6$` (sha512crypt) password hash set in `/etc/shadow` — **not** the empty /
  blank dev password the stock zeus image often ships.
- **Service set:** `multi-user.target.wants` enables the VanMoof units
  (`gateway`, `power`, `ride`, `ux`, `tracking`, `monitor`, `update`,
  `logging`, `mqtt-ftp`, `spi-can-bridge@bridge`, `spi-mqtt-bridge@ble`,
  `spi-mqtt-bridge@modem`, `vcan-starter`) alongside stock `mosquitto`,
  `avahi`, `iptables`, `busybox-syslog`, `ppp@nrf9160`.
- **Kernel:** stock i.MX8MN 5.4.70 config; the only extra loadable modules are
  `cryptodev.ko` and **`jailhouse.ko`** (the Jailhouse partitioning hypervisor
  — used to give the Cortex-M `imx8_bridge` its own partition). Wi-Fi
  (`linux-firmware-ath10k`) and `spidev` are present from the base BSP.
- **Persistence:** `/var/log` and `/var/lib/systemd/timesync` are bind-mounted
  from the ext4 config partition `mmcblk2p6/config/…`; mosquitto persistence
  lives there too. Everything else is read-only squashfs root (A/B).

## What was *not* changed

- The base distro identity (`/etc/os-release`, `/etc/version`) is left as the
  stock NXP zeus strings — VanMoof did not re-brand the OS.
- No custom kernel fork is evident in the rootfs (modules are the stock
  i.MX8MN set; only Jailhouse/cryptodev are added as out-of-tree modules).
- The firewall is effectively open: `/etc/iptables/iptables.rules` is **empty**
  (the `iptables.service` loads nothing). Network isolation relies on the
  mosquitto listener being bound to `lo` only, not on netfilter.
- `sshd` (OpenSSH) is installed but **not** in `multi-user.target.wants` — i.e.
  not auto-started in this production image. A serial getty (115200 8N1) is the
  enabled console.

## One-line summary

> `v1.5.0-main` = **NXP i.MX 5.4-zeus (2022 BSP, kernel 5.4.70, i.MX8M Nano)**
> + a **`meta-vmxs5` application layer**: one Go cloud gateway, a C++ service
> suite for power/ride/lock/theft/health/update, an MQTT-centric IPC bus with
> SPI/CAN/modem bridges, the bundled sub-ECU firmware, and VanMoof identity /
> config — all *added* to an otherwise stock NXP reference rootfs.
