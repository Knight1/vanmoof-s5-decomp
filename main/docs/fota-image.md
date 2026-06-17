# FOTA image format & eMMC layout

## Container

The `v1.5.0-main` file is three nested layers:

```
v1.5.0-main                         59,270,028 B  gzip (RFC1952, from Unix)
  └─ gunzip → tar                   59,576,320 B  POSIX tar (GNU)
       └─ member "VM-XS5_FOTA"      59,567,600 B  SquashFS 4.0 root filesystem
            └─ unsquashfs → rootfs                the i.MX8 Linux system
```

`sha256(v1.5.0-main) = 45ab7c4da9769c9a9079154493eeb6086f7e580f3660edcf77b5eba8cd878546`

### SquashFS superblock (`VM-XS5_FOTA`)

| Field | Value |
| --- | --- |
| Magic | `hsqs` (0x73717368) |
| Version | **4.0** |
| Compression | **gzip (zlib)** — id 1 |
| Block size | **131072** (128 KiB), block_log 17 |
| Inodes | 6190 |
| Contents | 5129 files, 386 dirs, 675 symlinks |

> `file(1)` mis-reports the superblock ("version 1024.0", absurd size) — it
> matches the wrong magic table. It *is* a normal SquashFS 4.0; `unsquashfs`
> reads it cleanly.

## Unpack recipe

```sh
# 1. gunzip
gunzip -k -c v1.5.0-main > main.tar          # → POSIX tar

# 2. untar (single member)
tar -xf main.tar                              # → VM-XS5_FOTA (squashfs)

# 3. unsquash
unsquashfs -d rootfs VM-XS5_FOTA              # → rootfs/  (the Linux tree)
```

(Equivalently `gunzip -c v1.5.0-main | tar -xO > VM-XS5_FOTA`.)

The extracted rootfs is **not** committed to this repo (clean-room policy — no
OEM binaries). Keep it outside the tree (e.g. a scratch dir) and analyze in
place.

## eMMC partition layout

The application processor boots from eMMC **`/dev/mmcblk2`**. Confirmed from
`/etc/fstab` and `/etc/fw_env.config`:

| Partition | FS | Mount | Role |
| --- | --- | --- | --- |
| `mmcblk2p1` | vfat | `/run/media/mmcblk2p1` | boot — U-Boot / kernel / dtb |
| `mmcblk2p6` | ext4 | `/run/media/mmcblk2p6` | **config / persistent** state |
| (root) | squashfs (ro) | `/` | this FOTA image, flashed to a root slot |
| — | — | U-Boot env | on `mmcblk2` at offset **0x400000**, size 0x1000 |

Persistent state bound into the read-only root from the config partition:

```
/run/media/mmcblk2p6/config/log       → /var/log
/run/media/mmcblk2p6/config/timesync  → /var/lib/systemd/timesync
/run/media/mmcblk2p6/config/mosquitto → mosquitto persistence
                       config/…       → provisioning, certs, settings
```

Volatile state is tmpfs: `/run`, `/var/volatile`.

> **A/B (inferred):** the root is a read-only SquashFS delivered as a single
> flashable blob, and the FOTA only carries the root image (boot/config are not
> in it). That is the classic dual-slot pattern — two root partitions, flash the
> inactive one, switch the U-Boot `boot_part` env var, fall back on failure —
> handled by `vmxs5-upgrade-scripts`. The exact slot partition numbers
> (`p2`/`p3` vs others) are **not** provable from the rootfs alone; confirm
> against an eMMC dump or the upgrade scripts before relying on them.

## Per-ECU device files

The sibling directory `../VanMooof-Firmware/SA5/v1.5.0-main_device_files/`
holds the individually-extracted peripheral images plus `manifest.txt` — the
same set bundled inside the rootfs at `/opt/devices_fw` (`vmxs5-device-binaries`).
These are the images the on-bike `update` service flashes to the sub-ECUs; each
has its own target subdirectory in this repo (`ble/`, `user_ecu/`, …).
