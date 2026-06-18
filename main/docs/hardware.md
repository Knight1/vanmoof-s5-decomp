# Hardware — i.MX8 main module

This is now grounded in the **device tree** shipped in the FOTA's kernel image
(`vm_mainecu-imx8mn-lpddr4.dtb`, extracted per [`fota-image.md`](fota-image.md)),
cross-checked against the sysfs paths the services open. The DTB's root model
string is:

> **`NXP VanMoof mainECU i.MX8MNano board`** — `compatible = fsl,imx8mn`

Kernel: **Linux 5.4.70+** (`oe-user@oe-host`, i.e. the Yocto build). RAM:
**LPDDR4** (from the DTB name).

> "okay" / "disabled" below are the node `status` from the DTB. Many i.MX8MN-EVK
> peripherals are carried over from the NXP reference DTS but **disabled** (not
> populated on the bike) — those are listed at the end so they aren't mistaken
> for real hardware.

## SoC & compute

- **NXP i.MX8M Nano** — quad **Cortex-A53** (`cpu@0..3`), GIC-v3, built
  `-mcpu=cortex-a53+crc+crypto`.
- **`jailhouse.ko`** (Siemens v0.12) + `jailhouse.bin` present for static
  partitioning; usage unconfirmed (no cell config in rootfs) — see
  [`kernel-modules.md`](kernel-modules.md). The SPI↔CAN bridge `imx8_bridge` is
  a *discrete* Cortex-M MCU, not a cell on this SoC.
- **CAAM** crypto (`caam-sm`, `caam-snvs`, 3 job rings) — i.MX hardware crypto +
  **SNVS** secure non-volatile storage (key store / secure-boot anchor);
  `cryptodev.ko` exposes it to userspace.

## SPI satellites — the wireless/secure front-end

The i.MX8 reaches four satellite chips over ECSPI using a **VanMoof custom
kernel driver `vm,mainecu_spi`** (all `status=okay`):

| Node | Part | Role |
| --- | --- | --- |
| `nrf52840@0` | Nordic **nRF52840** | **BLE** SoC (the [`ble/`](../../ble/) target); bridged to MQTT by `spi-mqtt-bridge@ble` |
| `nrf9160@1` | Nordic **nRF9160** | **LTE-M/NB-IoT + GNSS** cellular modem (the `modem` target); `spi-mqtt-bridge@modem` |
| `sr150@2` | NXP **SR150** (`nxp,sr1xx`) | **Ultra-Wideband (UWB)** secure ranging — precise phone-as-key distance/unlock |
| `lpc55sxx@0` | NXP **LPC55Sxx** (Cortex-M33 + TrustZone) | **secure element / secure co-processor** (key storage, immobilizer / secure boot helper) |

> The **UWB (SR150)** and the **LPC55 secure MCU** are new vs the earlier
> service-only view: neither shows up as an MQTT bridge, so they're driven
> through kernel/other paths, not the `spi-mqtt-bridge` pair.

## I²C — power, sensors, audio amp

| Node | I²C addr | Part | Role |
| --- | --- | --- | --- |
| `bq25672@6b` | 0x6b (`/sys/…/2-006b`) | TI **BQ25672** | LiPo charger / power-path; ship-mode regs 0x11/0x14 |
| `bq27542g1@55` | 0x55 (`/sys/class/power_supply/bq27542-0`) | TI **bq27542** | LiPo fuel gauge (SoC/V/I/temp/cycles/health) |
| `icm42600@68` | 0x68 | InvenSense **ICM-42600** (alt 43600), 6-axis | accelerometer+gyro — **wake-on-motion** / anti-theft |
| `ist8306@19` | 0x19 | Isentek **IST8306**, 3-axis | **magnetometer** (compass / orientation) |
| `tas2562@4c` | 0x4c | TI **TAS2562** mono Class-D | **speaker amplifier** — the bike's sounds / bell (`ux/sound/play`) |

## Two batteries (managed by `power`)

| Battery | Transport | Topics |
| --- | --- | --- |
| **LiPo** (internal/standby) | I²C (bq27542 + BQ25672 above) | `power/battery/lipo/info/*` |
| **primary** (removable Panasonic pack) | **CAN** (SocketCAN) | `power/battery/primary/info/*` |

## Connectivity, storage, I/O

- **Wi-Fi:** **Broadcom BCM4329-class** SDIO (`bcrmf@1`, `brcm,bcm4329-fmac`) on
  an SDHC controller. (The base image *also* ships `linux-firmware-ath10k`, but
  the populated radio in the DTB is the Broadcom part.)
- **Ethernet:** FEC `ethernet@30be0000` + RGMII PHY (`okay`) — wired diag/factory.
- **eMMC:** `mmc@30b60000` (`/dev/mmcblk2`, A/B layout). SD slot
  `mmc@30b50000` is **disabled**.
- **QSPI-NOR:** Micron **MT25QU256** (32 MB, `jedec,spi-nor`) on FlexSPI (`okay`)
  — bootloader / recovery / env store. **NAND** controller is **disabled**.
- **USB-C:** `nxp,ptn5110` **PTN5110** Type-C Port Controller (USB-PD) — the
  charge/phone-power port (matches the `user_ecu` USB-C monitor).
- **UARTs:** `serial@30880000`, `30890000`, `30a60000` (`okay`); the modem PPP
  link is on `/dev/ttymxc2`.
- **RTC** `/dev/rtc0` (wake alarms), **GPIO** via `/sys/class/gpio`, **TMU**
  thermal, **watchdog@30280000** (`okay`).
- **CAN** `vcan0` (SocketCAN), physical CAN behind the discrete Cortex-M
  `imx8_bridge` MCU over SPI (`spi-can-if-linux`; likely the `lpc55sxx`
  satellite — see [`../../imx8_bridge/`](../../imx8_bridge/)).

## Present-but-disabled (inherited from the NXP i.MX8MN EVK DTS)

Listed so they aren't mistaken for bike hardware: the GPU (`gpu@38000000`), the
EVK 8-channel audio codecs (`ak4458`, `ak4497`, `ak5558`), `micfil`/`spdif`/most
`sai`, all `pwm@…`, the `tca6416` GPIO expander, the SD-card slot, the GPMI
**NAND** controller, and ECSPI3 (`spi@30840000`) are all `status=disabled`.

## Identity / provisioning storage

Per-bike identity and AWS IoT credentials are **not** in the rootfs — they live
on the persistent config partition under `/run/media/mmcblk2p6/bike_id/…` and
`…/config/…` (read by `gateway`'s `internal/bike/provisioning`): the device
certificate/key for mutual-TLS, the bike's thing identity, and the IoT endpoint
host.
