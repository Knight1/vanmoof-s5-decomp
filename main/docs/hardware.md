# Hardware — i.MX8 main module

Peripherals are inferred from the rootfs (sysfs paths the services touch, kernel
modules, device-tree/driver names, and the helper scripts). Bus addresses are
the actual paths the C++ services open.

## SoC & compute

- **NXP i.MX8M Nano** — quad **Cortex-A53** (ARMv8-A), built
  `-mcpu=cortex-a53+crc+crypto`. Pinctrl `pinctrl_imx8mn` is built into the
  kernel; PHYs `phy_fsl_imx8mq_usb`, `phy_fsl_imx8_pcie` present.
- **Cortex-M co-core** runs `imx8_bridge` (the SPI↔CAN gateway firmware in
  `/opt/devices_fw`); **`jailhouse.ko`** partitions the SoC so the M-core / RT
  side is isolated from Linux.
- **eMMC** `/dev/mmcblk2` (boot0/boot1 + p1–p6; see `fota-image.md`).
- **Wi-Fi**: `linux-firmware-ath10k` present (Qualcomm Atheros) — radio for
  service/diagnostics; cellular is the primary uplink.

## I²C bus 2 — power management ICs

| Device | I²C addr (sysfs) | Part | Role |
| --- | --- | --- | --- |
| Charger | `2-006b` (`/sys/bus/i2c/devices/2-006b/{registers,gpios}`) | **TI BQ25672** | LiPo (internal battery) charger / power path; ship-mode via regs `0x11`,`0x14` |
| Fuel gauge | `2-0055` (`/sys/class/power_supply/bq27542-0`) | **TI bq27542** | LiPo gauge (SoC, voltage, current, temp, cycles, health); hibernate via `subcommandcode` |

> `shipping_mode.sh` references both a `BQ25672` and a `BQ25762`; the live
> sysfs node is `2-006b`. The comment block documents ship-mode: ensure
> VAC1/VAC2/VBUS absent, set `SDRV_CTRL` ship bits, hibernate the gauge, then
> `poweroff` (kernel driver drops `BATFET`).

## Two batteries

The `power` service manages two independent energy sources:

| Battery | Transport | Topics | Notes |
| --- | --- | --- | --- |
| **LiPo** (internal/standby) | I²C (gauge+charger above) | `power/battery/lipo/info/*` — `soc`, `voltage`, `current_now`, `temp`, `capacity`, `cycles`, `health`, `charge_full[_design]`, `status`, `pwr_avg` | keeps the Linux side + standby alive |
| **primary** (main Panasonic pack) | **CAN** (SocketCAN) | `power/battery/primary/info/*` — `soc`, `soc_app`, `voltage`, `temperature`, `charge_current`, `discharge_current`, `max_current`, `power`, `cycles`, `health` | the removable traction battery; BMS is the `battery_primary_panasonic` target |

## Other peripherals

- **RTC** `/dev/rtc0` (`rtc_handler`) — wake alarms for scheduled wake / deep
  sleep.
- **IMU** `/sys/bus/iio/devices/iio:deviceN` (`wake_on_motion_handler`,
  `common/imu`) — accelerometer; wake-on-motion (anti-theft / auto-wake).
- **GPIO** via `/sys/class/gpio` (`switch_control`, `gpio_func.sh`) — power
  switches, enables; group×32+pin numbering.
- **CAN** `vcan0` (SocketCAN, brought up by `vcan-starter`); physical CAN is on
  the Cortex-M `imx8_bridge` reached over SPI by `spi-can-if-linux`.
  `/sys/class/net/vcan0/statistics` is monitored.
- **Serial / SPI** — `serial_port` (`common/src/serial_port`) carries the **SSP**
  motor protocol (`ride`); `spidev` (from `kernel-module-spidev`) backs the BLE
  and modem SPI links (`spi-mqtt-bridge`).
- **`cryptodev.ko`** — userspace crypto offload to the i.MX CAAM.

## Power states (managed by `power`)

`state_manager` / `low_power` / `power_control` sequence: operational ·
charging · standby · **low_power** (`power/low_power`, `…_extend`) · **deep_sleep**
(`power/deep_sleep`) · **shipping** (ship-mode ICs, lowest leakage). Wake sources
are RTC alarm and IMU wake-on-motion. `eshifter_calibration` and an `rtc_handler`
live here too.

## Identity / provisioning storage

Per-device identity and AWS IoT credentials are **not** in the rootfs image —
they live on the persistent config partition under
`/run/media/mmcblk2p6/bike_id/…` and `…/config/…` (read by `gateway`'s
`internal/bike/provisioning`). This includes the device certificate/key for
mutual-TLS to AWS IoT and the bike's thing identity; the IoT endpoint host is
provisioned there rather than hard-coded.
