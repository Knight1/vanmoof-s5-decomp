# Version identification

## What this image is

This is **VanMoof S5/A5 main-module firmware `v1.5.0-main`**, the `production`
build, for the i.MX8 application processor. The FOTA bundle on disk is named
`v1.5.0-main`; on the device this is the rootfs of the main ECU.

Canonical on-image markers:

| File | Value |
| --- | --- |
| `/etc/firmware_version` | `v1.5.0-main` |
| `/etc/firmware_imagetype` | `production` |
| `/etc/hostname` | `vmxs5mainecu` |
| FOTA member name | `VM-XS5_FOTA` |
| `/opt/devices_fw/manifest.txt` | release `1.5.0`, build `20240129 14:52:22` |

The `1.5.0.main` / `v1.5.0-main` tag and the `20240129` (2024-01-29) build
stamp match the peripheral images in the same FOTA (`ble`, `user_ecu`,
`elock`, …) — i.e. this Linux image and the whole sub-ECU fleet ship as one
versioned release. See the release manifest table in the repo root `README.md`.

## OS base (the "OEM" underneath)

The Linux distribution is the **stock NXP i.MX reference image**, unmodified in
its identity files:

```
/etc/os-release
  ID="fsl-imx-xwayland"
  NAME="NXP i.MX Release Distro"
  VERSION="5.4-zeus (zeus)"
  VERSION_ID="5.4-zeus"
  PRETTY_NAME="NXP i.MX Release Distro 5.4-zeus (zeus)"

/etc/version    20221001000000
/etc/timestamp  20221001000000
/etc/issue      NXP i.MX Release Distro 5.4-zeus
```

- **Yocto release:** `zeus` (Yocto 3.0 / OpenEmbedded, 2019), NXP i.MX BSP
  `5.4-zeus`.
- **BSP build timestamp:** `2022-10-01` (the base image was assembled then; the
  VanMoof app layer on top was rebuilt for the 2024-01-29 release).
- This means the OS base is **frozen at the 2022 zeus BSP** while the VanMoof
  application layer kept moving — see `oem-differences.md`.

## Component versions

| Component | Version | Notes |
| --- | --- | --- |
| Linux kernel | **5.4.70+** | `/lib/modules/5.4.70+`; most drivers built-in, only `cryptodev.ko` + `jailhouse.ko` loadable |
| C library | **glibc 2.30** | `/lib/libc-2.30.so` |
| Init | **systemd** | unit files under `/lib/systemd/system` |
| BusyBox | **1.31.0** | syslog/klogd + applets |
| MQTT broker | **mosquitto 1.6.7** | the internal bus broker |
| SoC | **i.MX8M Nano** | `pinctrl_imx8mn` built-in; Cortex-A53 (`-mcpu=cortex-a53+crc+crypto`) |

## VanMoof build provenance

The Go `gateway` binary embeds its build metadata (the only target that does;
the C++ apps are stripped of VCS info):

```
path     github.com/VanMoof/embedded/gateway
mod      github.com/VanMoof/embedded/gateway  (devel)
toolchain go1.19, GOOS=linux GOARCH=arm64, CGO_ENABLED=1
vcs       git, revision 02f6cc72ae7aa36fcabf565d726a76f439e13a07
          time 2023-07-12T10:23:51Z, modified=true (dirty tree)
recipe    aarch64-poky-linux/vmxs5-gateway/1.0-r0
```

So VanMoof develops the application stack in a monorepo
`github.com/VanMoof/embedded`, and packages each app as a Yocto recipe
(`vmxs5-gateway`, `vmxs5-embedded-power`, …) in a `meta-vmxs5` layer that sits
on the NXP zeus BSP. The `gateway` was built from a **dirty** working tree
(`vcs.modified=true`) at git `02f6cc7` (2023-07-12); the release as a whole is
stamped 2024-01-29.

> The `Maintainer:` field on the `vmxs5-*` dpkg records is
> `NXP <lauren.post@nxp.com>` — that is the NXP BSP template's default
> maintainer carried into the custom recipes, **not** evidence the packages are
> NXP's. The package *contents* (`/usr/bin/gateway`, `…/power`, …) are VanMoof's.
