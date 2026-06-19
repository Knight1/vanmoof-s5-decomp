# FOTA image format & eMMC layout

## Container

`v1.5.0-main` is a **Pegatron (PEGA) whole-system FOTA image**, gzip-then-tar
wrapped. It is *not* rootfs-only — it carries the bootloader, kernel and rootfs
concatenated, with a 2 KiB descriptor trailer:

```
v1.5.0-main                         59,270,028 B  gzip (RFC1952, from Unix)
  └─ gunzip → tar                   59,576,320 B  POSIX tar (GNU)
       └─ member "VM-XS5_FOTA"      59,567,600 B  PEGA FOTA = [rootfs][kernel][boot][2KiB header]
```

`unsquashfs VM-XS5_FOTA` works directly only because the **rootfs squashfs sits
first** (offset 0); the tool reads the leading filesystem and ignores the
appended kernel + bootloader + trailer.

`sha256(v1.5.0-main) = 45ab7c4da9769c9a9079154493eeb6086f7e580f3660edcf77b5eba8cd878546`

### The PEGA header (last 2048 bytes; 643 used)

The on-device updater reads the image's tail to learn the layout. For this
build:

| Field | Value |
| --- | --- |
| `HEAD.HSIG` / `HEAD.TSIG` | `HEAD_PEGA_FOTA_VM-XS5_SIG` / `TAIL_PEGA_FOTA_VM-XS5_SIG` |
| `PRODUCT.VERSION` | `v1.5.0-main` |
| `SCRIPT.VERSION` | `1.2.9` (the `runFOTA` that *built* it; installed one is 1.3.11) |
| `FILE.ROOTFS` | `root.sqfs` @ **0**, size **48,914,272**, md5 `ef2bdfbf…` |
| `FILE.KERNEL` | `boot.sqfs` @ **48,914,272**, size **9,969,504**, md5 `dfe3c2cc…` |
| `FILE.BOOT` | `imx-boot_signed.tgz` (→ `imx-boot_signed.bin`) @ **58,883,776**, size **681,776** |
| `CORE.MD5` / `IMAGE.MD5` / `DTS.MD5` | product-core / kernel-image / device-tree digests (version gate) |
| `HEAD.MD5` | digest over the 643-byte header |

So the three payloads are: a **rootfs** SquashFS, a **kernel** packaged as its
own SquashFS (`boot.sqfs`, mounted to provide Image + dtb), and a **signed
i.MX bootloader** (`imx-boot_signed.bin` = SPL + U-Boot + ATF; "signed" ⇒
HAB/secure-boot). Everything is integrity-checked by **MD5** (not a
cryptographic signature at the FOTA layer — the *bootloader* is separately
signed for HAB).

### Rootfs SquashFS superblock

| Field | Value |
| --- | --- |
| Magic / version | `hsqs` / **4.0** |
| Compression | **gzip (zlib)** |
| Block size | 131072 (128 KiB) |
| Contents | 5129 files, 386 dirs, 675 symlinks (6190 inodes) |

> `file(1)` mis-reports the superblock; it *is* a normal SquashFS 4.0.

## Unpack recipe

```sh
gunzip -c v1.5.0-main | tar -xO > VM-XS5_FOTA     # the PEGA image

# rootfs (leading squashfs):
unsquashfs -d rootfs VM-XS5_FOTA

# the appended payloads, using the trailer offsets (byte-granular dd):
tail -c 2048 VM-XS5_FOTA                            # read the header
dd if=VM-XS5_FOTA of=root.sqfs   bs=4M iflag=skip_bytes,count_bytes skip=0        count=48914272
dd if=VM-XS5_FOTA of=boot.sqfs   bs=4M iflag=skip_bytes,count_bytes skip=48914272 count=9969504   # kernel
dd if=VM-XS5_FOTA of=imxboot.tgz bs=4M iflag=skip_bytes,count_bytes skip=58883776 count=681776    # bootloader
```

All three carved payloads MD5-match the PEGA header (`ef2bdfbf…`, `dfe3c2cc…`,
`f72d536d…`). What each contains (verified):

| Payload | Format | Contents |
| --- | --- | --- |
| `root.sqfs` | SquashFS 4.0 (gzip) | the Linux rootfs (5129 files) |
| `boot.sqfs` | SquashFS 4.0 (gzip) | the **kernel** — `Image` (22.5 MB, ARM64 **Linux 5.4.70+**) + `vm_mainecu-imx8mn-lpddr4.dtb` (the device tree; model *"NXP VanMoof mainECU i.MX8MNano board"*) |
| `imxboot.tgz` | gzip tar | `imx-boot_signed.bin` (3.4 MB; SPL + U-Boot + ATF, HAB-signed; inner md5 `eb60fb8e…`) |

So a slot is `[bootloader → boot.sqfs(kernel+dtb) → root.sqfs(rootfs)]`. The
device tree is the authoritative hardware map — see [`hardware.md`](hardware.md).
(The extracted payloads are **not** committed — clean-room policy.)

## eMMC layout & A/B "ping-pong"

The application processor boots from eMMC **`/dev/mmcblk2`**. The updater
(`/usr/bin/runFOTA.sh`, in `vmxs5-utils`) is a Pegatron A/B dual-slot
installer. Slots:

| Slot | Bootloader | Kernel | Rootfs |
| --- | --- | --- | --- |
| **A** (index 1) | `mmcblk2boot0` | `mmcblk2p2` | `mmcblk2p4` |
| **B** (index 2) | `mmcblk2boot1` | `mmcblk2p3` | `mmcblk2p5` |
| shared | — | — | `mmcblk2p6` = **user/config/persistent** (ext4); `mmcblk2p1` = vfat (misc) |

- **Bootloader** lives in the eMMC **hardware boot partitions** (`boot0`/`boot1`),
  written after clearing `/sys/block/mmcblk2bootN/force_ro`. The active one is
  selected with `mmc bootpart enable 1|2 0 /dev/mmcblk2`.
- **Current slot** is discovered from `/proc/cmdline` (`root=/dev/mmcblk2p4` ⇒
  slot A, `…p5` ⇒ slot B); the installer always writes the *other* slot, then
  flips the boot partition — classic flash-inactive-then-switch.
- **U-Boot env** is raw on `mmcblk2 @ 0x400000` size 0x1000 (`/etc/fw_env.config`),
  read/written with `fw_printenv`/`fw_setenv`.
- Each image is MD5-verified before and after write; if the device already holds
  the target md5, the write is skipped.

### Safe-update (anti-brick) state machine

`runFOTA.sh -s {install|verify|rollback|query}` tracks an install across
reboots in the U-Boot env var **`su_state`**, formatted `su_v1_` + 4 digits
`state,pp,pp,state` (the last two mirror the first two as a checksum):

```
state: 0 idle · 1 installed · 2 try-new · 3 failed · 4 rollback
pp:    0 @first(boot0) · 1 @second(boot1)
```

Flow: `install` writes the new slot + sets `installed`; on next boot `verify`
confirms the bike came up on the candidate (→ `try-new`/commit) or `rollback`
flips back to the previous boot partition. This is what makes a bad OTA
self-heal instead of bricking.

### Delta updates

If the header carries `DELTA.*` fields, the installer reconstructs the full
image with **`xdelta3`** against a backup of the previous FOTA kept on the user
partition (`mmcblk2p6/delta-update/VM-XS5_FOTA`), then proceeds normally. This
build ships no delta section (full image).

### Download transports

`runFOTA.sh -l <url>` supports `scp://`, `tftp://`, `http://` (factory/bench
paths). In production the image is delivered to `/tmp/download/VM-XS5_FOTA`
(cloud → `gateway`/`update`), and `runFOTA` is driven by the `update` service's
`runfota` client — see [`update.md`](update.md).

## Per-ECU device files (`/opt/devices_fw`)

The sub-ECU firmware (`ble`, `modem`, `user_ecu`, `battery_primary_*`, …) is **not
delivered separately** — it is baked into the **main rootfs** at `/opt/devices_fw`
(alongside `manifest.txt`), and ships in the same FOTA. The build also drops a
sibling copy at `…/v1.5.0-main_device_files/` (the same bytes). The on-bike
`update` service reads this dir and flashes each ECU over CAN/SMP; during an OTA
it reads the *new* set from the freshly-mounted rootfs at
`/tmp/root.sqfs/opt/devices_fw`.

### Clean extraction from the FOTA

`unsquashfs VM-XS5_FOTA` "works" but is **not clean** — the PEGA image has the
kernel + bootloader + 2 KiB trailer appended after the rootfs, so the tool reads
past EOF of the filesystem. Carve `root.sqfs` to its exact header length first:

```sh
gunzip -c v1.5.0-main | tar -xO > VM-XS5_FOTA
dd if=VM-XS5_FOTA of=root.sqfs bs=4M iflag=skip_bytes,count_bytes skip=0 count=48914272
unsquashfs -d rootfs root.sqfs                 # clean, no trailing-data noise
ls rootfs/opt/devices_fw/                       # the per-ECU .bin + manifest.txt
```

### Per-ECU `.bin` container formats

Each `.bin` is the **whole flashable image** (the updater streams it page-by-page;
the header is *inside* the image at a fixed offset, not a wrapper to strip):

| Device(s) | Container | Magic / marker |
| --- | --- | --- |
| Cortex-M ECUs — `user_ecu`, `imx8_bridge`, `elock`, `eshifter`, `frontlight`, `rearlight`, `motor_sensor`, `power_control`, `power_pedal` | **`VMFW`** header at file `0x134` (after the vector table) | `"VMFW"` |
| `ble`, `modem` (Nordic) | **MCUboot** image header at offset 0 | `0x96F3B83D` |
| `motor_control` | non-standard header | `0x000008AA` |
| `battery_primary_{panasonic,dynapack}`, `charger_liteon_*` | raw payload; flashed by `ThirdPartyUpdateClient` page-CRC (no internal magic) | — |

### The `VMFW` header (verified — `user_ecu` `0x134`)

```c
struct vmfw_header {            /* at file offset 0x134, right after the Cortex-M vector table */
    char     magic[4];         /* 0x134  "VMFW"                                              */
    uint32_t version;          /* 0x138  packed semver — SAME packing as manifest.txt:       */
                               /*        (major<<24)|(minor<<16)|patch|(variant<<13)         */
                               /*        0x01056000 = 1.5.0 main  (major1 minor5 patch0 variant3) */
    uint32_t crc32;            /* 0x13c  image CRC32 (target validates via its HW CRC engine, */
                               /*        e.g. user_ecu @0x40095000, seed 0xFFFFFFFF)          */
    uint32_t image_length;     /* 0x140  total image size in bytes — EXACTLY the file size    */
                               /*        (0x0001A88C = 108684 for user_ecu)                   */
    char     build_date[12];   /* 0x144  __DATE__   "Jan 29 2024"                             */
    char     build_time[9];    /* 0x150  __TIME__   "14:50:32"                                */
};
```

Two cross-checks make this authoritative: `image_length` (0x140) equals the file
size to the byte, and the `version` word (0x138) is byte-for-byte the
`manifest.txt` packing reconstructed in
[`../update/src/manifest.c`](../update/src/manifest.c) — so the updater can verify
each `.bin`'s embedded version against the manifest row before flashing.

Parse/validate one in three lines:

```python
import struct
b = open("user_ecu.….bin","rb").read()
magic, ver, crc, length = struct.unpack_from("<4sIII", b, 0x134)
assert magic == b"VMFW" and length == len(b)        # clean image
maj,minr,pat,var = ver>>24 & 0xff, ver>>16 & 0xff, ver & 0x1fff, ver>>13 & 0xff  # 1.5.0 / variant 3=main
```

Their flashing protocol (page-CRC over CAN / SMP / DFU) is in
[`update.md`](update.md) and reconstructed in [`../update/src/`](../update/src/);
each ECU also has its own target dir in this repo.
