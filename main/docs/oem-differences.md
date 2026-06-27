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
| `vmxs5-embedded-lightweight-update` | `/usr/bin/lightweight_update` | **standalone CLI flasher** — trimmed update path (one file → one device over CAN/tty), reusing the `update` clients. Reconstruction: [`../lightweight_update/`](../lightweight_update/). |
| `vmxs5-embedded-motor-update-lib` | (lib) | motor-controller update routines. |
| `vmxs5-modem-update` | (modem fw flasher) | nRF9160 modem firmware update (`mfw_nrf9160_1.3.1.zip`). |
| `vmxs5-embedded-logging` | `/usr/bin/logging` | **Logging service** (C++) — collects logs off the bus to `/var/log` (eMMC-backed). |
| `vmxs5-embedded-mqtt-ftp` | `/usr/bin/mqtt-ftp-service` | **File transfer over MQTT** — pushes/pulls files via the bus (log/cert/config transport). |
| `vmxs5-embedded-spi-mqtt-bridge` | `/usr/bin/spi-mqtt-bridge` | **SPI ↔ MQTT** bridge, instantiated for `ble` (nRF52840 @spidev0.0) and `modem` (nRF9160 @spidev0.1) — source/sink of the `ble/*`+`modem/*` namespace; per-connection `SPIMQTTClient`, `bridge/subscribe` forwarding. Reconstruction: [`../spi_mqtt_bridge/`](../spi_mqtt_bridge/). |
| `vmxs5-embedded-spi-can-bridge` | `/usr/bin/spi-can-if-linux` | **SPI ↔ CAN** bridge — exposes the CAN fleet on SocketCAN `vcan0` by bridging it over SPI (`/dev/spidev1.0`) to the `imx8_bridge` co-processor; CAN-TP multiframe reassembly. Reconstruction: [`../spi_can_bridge/`](../spi_can_bridge/). |
| `vmxs5-vcan-starter` | `/usr/sbin/start_vcan.sh` | brings up the `vcan0` virtual CAN interface at boot. |
| `vmxs5-input-event` | `/etc/udev/rules.d/50-input-event.rules` | **udev rules** — stable symlinks for the kernel input devices: `input/spi0.0` (nRF52840), `input/spi0.1` (nRF9160), `input/spi1.0` (imx8_bridge), and the **`input/pwr-btn`** = the BQ25672 charger's power-button input (consumed by `ux`). Config only. |
| `vmxs5-device-binaries` | `/opt/devices_fw/*` | **the sub-ECU firmware bundle** — every peripheral image (`ble`, `user_ecu`, `elock`, motors, lights, …) + `manifest.txt`, staged for `update`. |
| `vmxs5-embedded-mosquitto-conf` | `/etc/mosquitto/*` | the broker config + the **role-based ACL** (`mqtt-bus.md`). |
| `vmxs5-embedded-ppp-conf` | `/etc/ppp/peers/nrf9160` | the **cellular PPP** peer config for the nRF9160 modem: `/dev/ttymxc2` @ 115200, `nocrtscts`, `noauth`, `defaultroute`+`replacedefaultroute`, LCP echo 5 s / 3 fails. Used by `ppp@nrf9160.service`. Config only. |
| `vmxs5-upgrade-scripts` | `/pre-install.sh`, `/post-install.sh` | A/B-image-swap FOTA hooks — but in v1.5.0 these are **no-op stubs** (`#!/bin/sh` + `echo {pre,post} install stub`). The real A/B swap + install logic lives in `update` / `runFOTA.sh` (the `update` service shells out to these hooks around the i.MX8 self-update). |
| `vmxs5-utils` | `/usr/bin/{runFOTA.sh, gpio_func.sh, shipping_mode.sh}` | **Shell utilities.** `runFOTA.sh` = the i.MX8 **FOTA / A-B image-swap orchestrator** (2029-line Pega ODM script v1.3.11: parses a 2048-byte `HEAD_PEGA_FOTA_VM-XS5_SIG` header with BOOT/KERNEL/ROOTFS/DELTA labels, `dd`-splits the image, writes the **off-line A/B slot** (`mmcblk2boot0/1` + parts p2/p3 kernel, p4/p5 rootfs, p6 user) with md5-skip, swaps via `mmc bootpart enable`, supports xdelta3 **delta** updates and a **safe-update** state machine via `fw_setenv su_state`) — the `update` service shells out to it. `shipping_mode.sh` = waits for VBUS/VAC1/VAC2 absent (BQ25672 `CHRG_STAT_0` @ i²c `2-006b`), sets the **bq27542 gauge to HIBERNATE** (`00 11` → `2-0055/subcommandcode`) and `poweroff` (kernel drops BQ25672 into Ship Mode). `gpio_func.sh` = sysfs GPIO helpers. |
| `vmxs5-version` | `/etc/firmware_version`, `/etc/firmware_imagetype` | version stamps — `v1.5.0-main` / `production` (config only). |

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
- **Kernel:** the NXP zeus **5.4.70** kernel (no fork), but with a **custom
  VanMoof board port** — its own device tree `vm_mainecu-imx8mn-lpddr4.dtb`
  (model *"NXP VanMoof mainECU i.MX8MNano board"*) and a custom SPI driver
  `vm,mainecu_spi` binding the four SPI satellites (nRF52840, nRF9160, **SR150
  UWB**, **LPC55Sxx** secure MCU), plus drivers for the bike's I²C parts
  (BQ25672, bq27542, ICM-42600, IST8306, TAS2562). The EVK-default peripherals
  (GPU, EVK audio codecs, NAND, SD) are carried over from the NXP DTS but
  `status=disabled`. Extra loadable modules: `cryptodev.ko` and
  **`jailhouse.ko`** (partitions the Cortex-M `imx8_bridge`).
- **Persistence:** `/var/log` and `/var/lib/systemd/timesync` are bind-mounted
  from the ext4 config partition `mmcblk2p6/config/…`; mosquitto persistence
  lives there too. Everything else is read-only squashfs root (A/B).

## What was *not* changed

- The base distro identity (`/etc/os-release`, `/etc/version`) is left as the
  stock NXP zeus strings — VanMoof did not re-brand the OS.
- No custom kernel *fork* — it's the stock NXP 5.4.70 tree with a VanMoof board
  DTS + drivers added (a board port, not a fork); Jailhouse/cryptodev are the
  only out-of-tree loadable modules.
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
