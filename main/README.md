# `main` — i.MX8 application processor (Linux)

The **main module** of the VanMoof S5 / A5 (`XS5`). It is the brain of the
bike: a full embedded **Linux** system on an **NXP i.MX8M Nano** (Cortex-A53)
that talks to *everything* — the BLE SoC, the cellular modem, and the whole CAN
fleet of Cortex-M sub-ECUs — and bridges the bike to the cloud (AWS IoT).

Hostname: **`vmxs5mainecu`**. Firmware version: **`v1.5.0-main`** (production,
built 2024-01-29).

Unlike the other targets in this repo, this one is **not** a bare-metal MCU
decomp. It is a Linux root filesystem: a stock **NXP i.MX "zeus"** Yocto base
with a VanMoof application layer (`meta-vmxs5`) bolted on top. The work here is
**analysis and documentation** of that layer — what services run, how they talk,
and exactly what VanMoof added to the OEM image — not C reconstruction.

## The image

The reference image is the FOTA bundle
`../VanMooof-Firmware/SA5/v1.5.0-main/v1.5.0-main`:

```
v1.5.0-main            gzip
  └─ (tar)             POSIX tar, one member "VM-XS5_FOTA"
       └─ VM-XS5_FOTA  SquashFS 4.0 (gzip, 128 KiB blocks) — the root filesystem
```

Unpack recipe and on-eMMC layout are in [`docs/fota-image.md`](docs/fota-image.md).
The rootfs is **not** committed (clean-room policy — no OEM binaries in the
repo); extract it yourself from a FOTA bundle or eMMC dump.

## Documentation

| Doc | Covers |
| --- | --- |
| [`version.md`](docs/version.md) | **what version this is** — firmware tag, OS base, kernel/toolchain, build provenance |
| [`oem-differences.md`](docs/oem-differences.md) | **what VanMoof changed vs the stock NXP i.MX image** — the `meta-vmxs5` layer, package-by-package |
| [`architecture.md`](docs/architecture.md) | the whole stack — SoC, boot/partitions, the service graph, how data flows bike ↔ cloud |
| [`services.md`](docs/services.md) | per-service / per-binary inventory (the Go `gateway`, the C++ app suite, the bridges) |
| [`mqtt-bus.md`](docs/mqtt-bus.md) | the internal **MQTT** IPC bus — topic namespace + the BLE role-based ACL |
| [`fota-image.md`](docs/fota-image.md) | container format, unpack recipe, eMMC partition map |
| [`progress.md`](docs/progress.md) | analysis tracker |

## At a glance

- **SoC:** NXP **i.MX8M Nano** (quad Cortex-A53). `jailhouse.ko` present (core
  partitioning); `imx8_bridge` runs on the Cortex-M co-core as the SPI↔CAN gateway.
- **OS:** NXP i.MX Release Distro **5.4-zeus** (Yocto 3.0 "zeus"), kernel
  **5.4.70**, glibc **2.30**, systemd, BusyBox 1.31, mosquitto **1.6.7**.
- **IPC:** a loopback **mosquitto** MQTT broker is the internal message bus.
  Every app and every external link (BLE, modem, CAN) is an MQTT client.
- **Cloud:** the Go **`gateway`** speaks **AWS IoT** (Device Shadow + Jobs +
  telemetry rules) over the nRF9160 cellular PPP link.
- **VanMoof layer:** ~23 `vmxs5-*` Yocto packages — one Go service (`gateway`)
  plus a suite of C++ services (`power`, `ride`, `ux`, `tracking`, `monitor`,
  `update`, `logging`, `mqtt-ftp`) and the SPI/CAN/MQTT bridges.
