# Secure boot — HAB chain of trust (`imx-boot_signed.bin`)

Full decode of the i.MX8M Nano boot chain shipped in `v1.5.0-main`: the signed
bootloader container, its two HABv4 Command Sequence Files, the VanMoof PKI, and
the **custom U-Boot commands that extend the chain past U-Boot into the kernel
and rootfs**.

Everything below was derived from the image itself. All six signatures were
re-verified offline (see [Verification results](#verification-results)) — the
chain is arithmetically sound, not merely plausible.

Container/eMMC context is in [`fota-image.md`](fota-image.md); this file covers
only the authentication layer.

## Subject

| | |
| --- | --- |
| File | `imx-boot_signed.bin` (from `imxboot.tgz`, `FILE.BOOT` of the PEGA FOTA) |
| Size | 3,406,976 B (`0x33FC80`) |
| md5 / sha256 | `eb60fb8e9c2552b6b71be5e182ca72c9` / `2ac8c3c2800381d426aee61c5a26a9b2ba9dd4babc6d046e00644a79f3f5fd80` |
| SoC | i.MX8M Nano (SPL text base `0x912000`, BL31 at `0x960000` ⇒ 8MN OCRAM map) |
| Format | `imx-mkimage` "flash.bin": stage-1 IVT+SPL+CSF, then FIT + stage-2 IVT+CSF |
| Written to | eMMC **hardware boot partition** `mmcblk2boot0`/`boot1` at offset 32 KiB |

Component versions (all NXP `imx_v2020.04_5.4.70_2.3.x` BSP, Yocto zeus):

| Component | Version |
| --- | --- |
| U-Boot SPL | `2020.04-5.4.70-2.3.3+g44f5949dd9` (Oct 01 2022) |
| U-Boot | `2020.04-5.4.70-2.3.3+g44f5949dd9` |
| ARM Trusted Firmware | `v2.2(release):rel_imx_5.4.70_2.3.2_rc1-0-g2a2678646-dirty` |
| OP-TEE | `rel_imx_5.4.70_2.3.2_rc1` |
| Toolchain | `aarch64-poky-linux-gcc 9.2.0`, binutils 2.32 |
| Board | `imx8mn_evk` derived; DTB `vm_mainecu-imx8mn-lpddr4.dtb` |

CMS signing timestamp on all four signatures: **2024-01-29 15:21:00 UTC** —
same day as the rest of the `v1.5.0-main` build.

## Byte map

Every non-zero byte of the file is under signature. The two gaps are pure
zero padding (verified byte-wise), so there is no unauthenticated payload
smuggled into the container.

| File range | Size | Contents | Covered by |
| --- | --- | --- | --- |
| `0x000000`–`0x024600` | 148,992 | IVT₁ + boot_data + `u-boot-spl.bin` + LPDDR4 PMU training firmware | **IMG sig 1** |
| `0x024600`–`0x024658` | 88 | CSF₁ command sequence | **CSF sig 1** |
| `0x024658`–`0x024A98` | 1,088 | SRK table | hashed vs. fuses |
| `0x024A98`–`0x024DEC` | 852 | CSF₁ CSF certificate (`CRT` blob) | chains to SRK[0] |
| `0x024DEC`–`0x024FF0` | 516 | CSF₁ CSF signature (`SIG`, CMS) | — |
| `0x024FF0`–`0x025344` | 852 | CSF₁ IMG certificate | chains to SRK[0] |
| `0x025344`–`0x025545` | 513 | CSF₁ IMG signature (CMS) | — |
| `0x025545`–`0x058000` | 207,547 | zero pad | — |
| `0x058000`–`0x059020` | 4,128 | FIT header (`0xD00DFEED`, 0x407 B) + IVT₂ | **IMG sig 2** (block 1) |
| `0x059020`–`0x059090` | 112 | CSF₂ command sequence | **CSF sig 2** |
| `0x059090`–`0x059F7D` | 3,821 | CSF₂ SRK table + 2 certs + 2 sigs | as above |
| `0x059F7D`–`0x05B000` | 4,227 | zero pad | — |
| `0x05B000`–`0x1291F0` | 844,272 | `u-boot-nodtb.bin` → `0x40200000` | **IMG sig 2** |
| `0x1291F0`–`0x130C60` | 31,344 | `u-boot.dtb` → `0x402CE1F0` | **IMG sig 2** |
| `0x130C60`–`0x13BE10` | 45,488 | `bl31.bin` (ATF) → `0x00960000` | **IMG sig 2** |
| `0x13BE10`–`0x33FC80` | 2,113,136 | `tee.bin` (OP-TEE) → `0x56000000` | **IMG sig 2** |

The DDR training firmware lives *inside* the signed stage-1 blob (`DDRINFO:`,
`ddr_pmu_train_imem/dmem` strings), so DRAM bring-up is authenticated too.

### IVTs

```c
struct ivt { u32 hdr, entry, rsv1, dcd, boot_data, self, csf, rsv2; };
```

| | IVT₁ (`0x000000`) | IVT₂ (`0x059000`) |
| --- | --- | --- |
| `hdr` | `0x412000D1` (tag D1, len 0x20, **v4.1**) | `0x402000D1` (v4.0) |
| `entry` | `0x00912000` (SPL, OCRAM) | `0x401FCDC0` (FIT base in DRAM) |
| `dcd` | 0 (DDR init is code, not a DCD) | 0 |
| `boot_data` | `0x00911FE0` → `{start 0x00911FC0, length 0x26660, plugin 0}` | 0 |
| `self` | `0x00911FC0` | `0x401FDDC0` |
| `csf` | `0x009365C0` (file `0x024600`) | `0x401FDDE0` (file `0x059020`) |

`boot_data.length = 0x26660` = signed region `0x24600` + `0x2060` of CSF blobs,
so the ROM copies exactly SPL **plus** its authentication material into OCRAM.

The FIT is *external-data* (`data-position`/`data-size`, no embedded payloads),
so CST hashed the sub-images at their **FIT file offsets** while the CSF records
their **DRAM load addresses**. Those two maps differ — a linear
`vaddr → file` translation of the CSF block list gives a wrong digest. The
correct offsets come from the FIT nodes:

| FIT node | `data-position` (+`0x58000`) | `load` |
| --- | --- | --- |
| `uboot@1` | `0x003000` → `0x05B000` | `0x40200000` |
| `fdt@1` | `0x0D11F0` → `0x1291F0` | (appended, `0x402CE1F0`) |
| `atf@1` | `0x0D8C60` → `0x130C60` | `0x00960000` |
| `tee@1` | `0x0E3E10` → `0x13BE10` | `0x56000000` |

## The CSFs

Both use CSF header **version 0x45** (HAB 4.5 / i.MX8M-family CST). Key and
signature pointers carry no `ABS` flag, so all locations are **offsets relative
to the CSF start**, not absolute addresses.

### CSF₁ — stage 1 (ROM authenticates SPL)

```
INS_KEY  pcl=SRK   alg=SHA256  src=0 tgt=0  flags=CLR   loc=CSF+0x058   # SRK table -> hashed vs fuses
INS_KEY  pcl=X509  alg=ANY     src=0 tgt=1  flags=CSF   loc=CSF+0x498   # CSF key, verified by SRK[0]
AUT_DAT  key=1     pcl=CMS     eng=CAAM                 sig=CSF+0x7EC   # authenticates the CSF itself
UNLK     eng=CAAM  feat=0x00000001                                      # MID: leave JR/DECO master-ID regs unlocked
INS_KEY  pcl=X509  alg=ANY     src=0 tgt=2  flags=CLR   loc=CSF+0x9F0   # IMG key, verified by SRK[0]
AUT_DAT  key=2     pcl=CMS     eng=CAAM                 sig=CSF+0xD44
         block 0x00911FC0 + 0x24600                                     # IVT + boot_data + SPL + DDR fw
UNLK     eng=CAAM  feat=0x00000002                                      # RNG: leave un-instantiated for U-Boot/OP-TEE
```

### CSF₂ — stage 2 (SPL authenticates the FIT)

Same key install sequence, then one `AUT_DAT` over five blocks:

```
AUT_DAT  key=2  pcl=CMS  eng=CAAM  sig=CSF+0xD5C
         block 0x401FCDC0 + 0x001020    FIT header + IVT2
         block 0x40200000 + 0x0CE1F0    u-boot-nodtb.bin
         block 0x402CE1F0 + 0x007A70    u-boot.dtb
         block 0x00960000 + 0x00B1B0    bl31.bin  (ATF)
         block 0x56000000 + 0x203E70    tee.bin   (OP-TEE)
UNLK     eng=CAAM  feat=0x00000002
```

SPL invokes this from `board_spl_fit_post_load()` — on failure it prints
`spl: ERROR:  image authentication unsuccessful` and hangs (string present in
the SPL blob at `0x99AE`).

Note the DTB is signed. Board configuration, including the OP-TEE and reserved
memory nodes, cannot be tampered with between SPL and U-Boot.

## The PKI

Stock NXP CST `hab4_pki_tree.sh` output, **unmodified**: default key CNs,
default serial-number series (`0x12345678`+), `OpenSSL Generated Certificate`
Netscape comment. Generated **2022-08-17 11:42:34 UTC**.

### SRK table — 4 × RSA-2048/e=65537, all flagged CA

The table is byte-identical in all four CSFs (bootloader ×2, kernel, rootfs).

```
SRK table SHA-256  =  c9b914a5 aeeb1708 60be688f 002cfc6f b0930cd4 8132cd3e fad4cf08 bb036675
```

That digest is what must be burned into **`SRK_HASH[0..7]`** for this bootloader
to be accepted by a closed part. As fuse words:

```
0xC9B914A5 0xAEEB1708 0x60BE688F 0x002CFC6F 0xB0930CD4 0x8132CD3E 0xFAD4CF08 0xBB036675
```

| # | modulus (head … tail) | SHA-256 of the SRK record |
| --- | --- | --- |
| 0 | `c846e2640537f68e977f3fab38fd57c6` … `1ceebaaa69ebef1f` | `876d8154f926d35ccd5ae9cd1bb247622cb4ea501c3ee9384681d4f9a2214139` |
| 1 | `c0ba556b52d4a556f0f5cc63143e2069` … `6111900099e84503` | `066a1fb849c516a9743c633d86765d3331abed4ea0d7a8443696ddaea5babbab` |
| 2 | `aff4599d91872ace1a8bab27fbed3e26` … `8e8ee7e0aeaff0cd` | `c49861d40ecebd6af1457d255eeeb470a6da0abdc9d1b2df2bb5e2c84d13da3b` |
| 3 | `c70fb01cce0a1ec7349c0f3fb797e1f9` … `419030d9b439928b` | `9263d32dd1b331288da38bb9d932f58a74cd3ffe09fdd453fcdfa798ec109802` |

Only **SRK[0]** is used (`src_idx=0` everywhere). SRK[1..3] are unused spares —
the intended rotation path is blowing `SRK_REVOKE` bits and re-signing against
the next index (indices 0–2 are revocable; index 3 is not).

### Leaf certificates

Both are issued by `CN=SRK1_sha256_2048_65537_v3_ca`
(AKI keyid `46:BA:8D:75:C6:C1:2C:33:3A:6B:FE:C0:8D:A2:B6:CC:FE:63:98:53`),
`CA:FALSE`, RSA-2048, `sha256WithRSAEncryption`, valid
**2022-08-17 → 2032-08-14**.

| Role | Subject CN | Serial |
| --- | --- | --- |
| CSF | `CSF1_1_sha256_2048_65537_v3_usr` | `0x12345679` |
| IMG | `IMG1_1_sha256_2048_65537_v3_usr` | `0x1234567A` |

```
CSF cert SHA-256  17415b146e35aae6eff66e8559e578ad9d50939291165d9fb761628a195968d0
IMG cert SHA-256  e432dd895cfc51d4d423e4d22d55466ead7bb78db8be5ab2cfa6c62ea6976d48
```

The **same two leaf keys sign everything** — SPL, the FIT, the kernel squashfs
and the rootfs squashfs. There is no per-stage or per-artifact key separation.

X.509 validity dates are decorative here: HAB has no trusted time source and
does not evaluate `notBefore`/`notAfter`. Expiry in 2032 has no runtime effect.

Signatures are detached CMS `SignedData` (`pkcs7-data`, digest SHA-256, RSA
PKCS#1 v1.5), no certificates embedded in the CMS, `signedAttrs` =
{contentType, signingTime, messageDigest}.

## Chain stages 3 & 4 — kernel and rootfs

This is the VanMoof-specific part. Stock NXP U-Boot stops authenticating after
itself; this build carries **five non-stock commands** that carry HAB all the
way to the rootfs:

| Command | Handler | Purpose |
| --- | --- | --- |
| `bootpp` | `0x402096E0` | boot the latest ping-pong slot, authenticating both partitions |
| `hab_auth_kernelfs [n]` | `0x40209194` | authenticate kernel squashfs (partition 2 or 3) |
| `hab_auth_rootfs [n]` | `0x40209070` | authenticate rootfs squashfs (partition 4 or 5) |
| `hab_auth_img_or_fail` | `0x40203E00` | `hab_auth_img`, but drop to BootROM USB SDP on failure |
| `hab_failsafe` | `0x402034CC` | call `hab_rvt_failsafe()` directly |

(`safe_u`, `eraseenv`, `rom_log`, `rom_log_dbg` are also non-stock; 113 commands
total.)

### Boot command selection

`main_loop()` (`0x40219390`) is stock, but `bootdelay_process()` (`0x4021B844`)
is **not** — it never calls `env_get("bootcmd")`. On a normal boot it returns a
hard-coded literal:

```
0x4021B8DC  adrp x19, #0x4029A000
0x4021B8E0  add  x19, x19, #0xE0B      ; "bootpp"   (standalone, NUL-terminated)
...
0x4021B950  mov  x0, x19
0x4021B95C  ret                        ; -> autoboot_command(s)
```

The full selection logic:

```
bootdelay = env_get("bootdelay") ? strtol(...) : -2        # CONFIG_BOOTDELAY = -2
bootdelay = <dtb override>                                 # absent from this DTB
if is_boot_from_usb():
        if env bootcmd_mfg  -> s = env_get("bootcmd_mfg")  # "Run bootcmd_mfg: %s"
        else                -> env_set("bootcmd","fastboot 0"); "Boot from USB for uuu"
else    "Normal Boot"
s = "bootpp"                                               # unless bootcmd_mfg took it
env_set kernaddr/rootaddr from dtb kernel-offset/rootdisk-offset
```

then `main_loop()` does `if (cli_process_fdt(&s)) cli_secure_boot_cmd(s);` —
the control DTB (signed, appended to `u-boot-nodtb.bin`) has **no `/config`
node**, so that override is inert — and `autoboot_command(s)` (`0x4021B9DC`):

| `stored_bootdelay` | Behaviour |
| --- | --- |
| `-1` | skip autoboot → straight to `cli_loop()` |
| `< 0` (default **-2**) | `run_command_list(s, -1, 0)` immediately, **no abort check** |
| `>= 0` | `"Hit any key to stop autoboot: %2d "` → keypress aborts into `cli_loop()` |

The stock NXP `bootcmd` still present in the default env (`loadimage` +
`mmcboot` + `booti`, with `mmcdev=1`) is therefore **dead** on this build — it
is only ever *written* (`bootcmd=fastboot 0`) in the uuu path, never read.

### Command-level gating

`booti` is **not** stock either. `bootpp` sets a one-byte global
`*(u8*)0x402CEAC0 = 1` immediately before issuing its `booti` (at `0x40209ADC`),
and `do_booti` (`0x4020B02C`) refuses to run without it:

```c
if (!bootpp_auth_passed()) {                        /* 0x40209C24: return *(u8*)0x402CEAC0 */
    printf("### ERROR ### Authentication hasn't passed in bootpp(), Please check\n");
    if (imx_hab_is_enabled())
        hang();                                     /* 0x4027FC98 — "Please RESET the board", then b . */
    return CMD_RET_FAILURE;
}
```

So on a **closed** part, `booti` typed at the console dead-stops the board.
`do_bootm` (`0x40208424`) is gated differently — by image format, via
`genimg_get_format()`:

| Format | Behaviour |
| --- | --- |
| `IMAGE_FORMAT_LEGACY` (uImage) | `authenticate_image_sqfs(addr, ih_size + 0x40)` — **must pass HAB** |
| `IMAGE_FORMAT_ANDROID` | proceeds, **no HAB check** (`ANDROID!` magic is compiled in) |
| `IMAGE_FORMAT_FIT` | refused — `Not valid image format for Authentication, Please check` |

There is no FIT signature fallback: the control DTB has no `/signature` node and
the binary contains no RSA verification code, so `CONFIG_FIT_SIGNATURE` is off.

**The gating is per-command, and `go` is not gated at all.** Only
`0x4020B0C8` (inside `do_booti`) calls `bootpp_auth_passed()`; the only callers
of the HAB routines are `hab_status`, `hab_auth_img_or_fail`, `hab_failsafe`,
`do_bootm` and `authenticate_partition`. `do_go` (`0x402082AC`) ends in a bare
indirect branch with no check of any kind:

```
0x40208298  mov x3, x0 / mov w0, w1 / mov x16, x3 / mov x1, x2
0x402082A8  br  x16                      ; "## Starting application at 0x%08lX ..."
```

`bootelf`, `bootvx`, `bootefi` and `bootaux` are likewise ungated. Anyone at the
U-Boot prompt therefore has arbitrary code execution at U-Boot's exception level
(EL2, non-secure) regardless of fuse state — the `booti` gate raises the bar for
the obvious command but does not close the shell as an escape. Secure world
(ATF at EL3, OP-TEE at `0x56000000`) is unaffected.

### Signed squashfs format

`boot.sqfs` and `root.sqfs` are not merely checksummed — each carries an
**appended HAB IVT + CSF** at the 4 KiB-aligned end of its squashfs payload:

| | `boot.sqfs` (kernel) | `root.sqfs` (rootfs) |
| --- | --- | --- |
| `bytes_used` | `0x980183` | `0x2EA4D6A` |
| IVT at | `0x981000` | `0x2EA5000` |
| IVT `self` / `entry` | `0x40E01000` / `0x40480000` | `0x43325000` / `0x40480000` |
| signed block | `0x40480000 + 0x981020` | `0x40480000 + 0x2EA5020` |
| `messageDigest` | `208db195aa31ddf9…217b8b52` | `0521950808de2839…3da315df` |

Both CSFs are the same 5-command sequence as CSF₂ (SRK → CSF key → CSF sig →
IMG key → IMG sig → `UNLK CAAM RNG`), with **the same SRK table hash and the
same two leaf certificates** as the bootloader.

### `authenticate_partition()` — `0x40208D80`

Called as `(partition, dry_run)`:

1. `mmc read 0x40480000 <part_lba> 0x1` — pull the squashfs superblock.
2. Check magic `hsqs`; else
   `### ERROR ### sqfs-magic=0x%x, partition %d is not a valid sqfs image` → `-3`.
3. `raw_size = superblock.bytes_used` (offset 40) →
   `### INFO ### sqfs partition %d raw_size is 0x%x`.
4. `ivt_offset = (raw_size + 0xFFF) & ~0xFFF`;
   `image_size = ivt_offset + 0x2020` (= `IVT_SIZE 0x20` + `CSF_PAD_SIZE 0x2000`).
5. `mmc read 0x40480000 <part_lba> <blocks>` — the whole authenticated extent.
6. `imx_hab_authenticate_image(0x40480000, image_size, ivt_offset)` via the
   thin wrapper at `0x40203E80`.
7. Non-zero → `### ERROR ### Authenticate partition %d Fail, Please check`.

### `bootpp` — `0x402096E0`

```
mmc partconf 2                                  → active eMMC boot partition (ping-pong slot)
read ROM event log (ptr @ OCRAM 0x9E0)          → (log >> 8) & 0xFF:
      0x15 "rom booted from primary"
      0x25 "rom booted from secondary"
      0x45 "rom booted from serial download mode"
read env su_state                               → sfu_state / rollback / sfu_pp
→ "### INFO ### sfu_state=%s, rollback=%d, sfu_pp=%d, ecsd=%d, is_bt_from_primary=%d"

slot 1: authenticate_partition(4)  then  authenticate_partition(2)
slot 2: authenticate_partition(5)  then  authenticate_partition(3)
        │
        ├─ both OK ─→ sqfsload mmc 2:<kpart> 0x40480000 Image
        │             setenv bootargs console=ttymxc1,115200 root=/dev/mmcblk2p6 \
        │                     rootfstype=squashfs rootrw=… rootrwfstype=ext4 maxcpus=1
        │             [+ " rootrwreset=yes" if env rootrwreset=yes]
        │             sqfsload mmc 2:<kpart> 0x43000000 vm_mainecu-imx8mn-lpddr4.dtb
        │             if (su_state == installed) → write su_state = try-new
        │             booti 0x40480000 - 0x43000000
        │
        └─ either fails ─→ "### ERROR ### Starting the FOTA roll-back"
                           write su_state = failed
                           mmc partconf 2 0 <other> 0        (flip boot partition)
                           reset
```

Rootfs/kernel partition mapping matches the Linux-side installer exactly
(slot A = p2/p4, slot B = p3/p5 — see [`fota-image.md`](fota-image.md)), and
U-Boot writes the *same* `su_state` word that `runFOTA.sh` reads:
`su_v1_` + `<state><pp><pp><state>` with state ∈
{0 idle, 1 installed, 2 try-new, 3 failed, 4 rollback} (`0x40208F5C`).

So the anti-brick state machine is genuinely split across U-Boot and userspace:
U-Boot promotes `installed → try-new` on a successful authenticated boot, and
demotes to `failed` + flips slots when authentication fails.

## How enforcement actually works

`imx_hab_authenticate_image()` (`0x40203964`) is unmodified NXP code. Its exit
path is the thing that matters:

```
0x402039D0  bl   imx_hab_is_enabled
0x402039D4  tst  w0, #0xff
0x402039D8  b.eq 0x40203CE8          ; not enabled -> result = 0
0x40203CE8  mov  w19, #0
```

i.e. the stock

```c
if ((!imx_hab_is_enabled()) || (load_addr != 0))
    result = 0;
```

**On a part whose `SEC_CONFIG` fuse is not blown, every `hab_auth_*` call
returns success regardless of whether HAB accepted the image.** The only visible
difference is the line `hab fuse not enabled` on the console. The same code (and
the same string) is present in SPL, so the SPL→FIT check behaves identically.

Everything in this document is therefore conditional on one fuse bit.
`imx_hab_is_enabled()` (`0x402035C0`) reads it as:

| SoC family (`get_cpu_rev() >> 12 & 0xF0`) | Fuse | Mask |
| --- | --- | --- |
| `0x80` (**i.MX8M**, incl. 8MN) | bank **1**, word **3** | `0x02000000` (bit 25) |
| `0x70` | bank 1, word 3 | `0x02000000` |
| `0xE0` | bank 1, word 3 | `0x80000000` |
| other | bank 1, word 3 | `0x00000002` |

Checking a live bike from the U-Boot console:

```
=> hab_status          # HAB Configuration: 0xcc = closed, 0xf0 = open; plus any HAB events
=> fuse read 1 3       # bit 25 (0x02000000) set ⇒ SEC_CONFIG blown ⇒ closed
=> rom_log             # ROM boot events, incl. which boot partition the ROM used
```

`hab_status` also dumps the HAB event log with decoded `ENG`/`CTX` fields — on
an open part that log is where rejected images show up while the boot continues
anyway.

Fuse state cannot be read out of a firmware image. Whether shipped S5/A5 units
are actually closed is **not determinable from these files** and needs a device
check.

## Verification results

All six signatures re-verified offline against the extracted SRK table:

| Artifact | CSF cert → SRK[0] | IMG cert → SRK[0] | CSF sig | IMG sig | digest matches content |
| --- | --- | --- | --- | --- | --- |
| stage 1 (SPL) | ✓ | ✓ | ✓ | ✓ | ✓ |
| stage 2 (FIT) | ✓ | ✓ | ✓ | ✓ | ✓ |
| `boot.sqfs` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `root.sqfs` | ✓ | ✓ | ✓ | ✓ | ✓ |

Image digests (CMS `messageDigest` == recomputed SHA-256 over the CSF block
list):

```
stage 1   ce7c3a94e705eca0f6e301b46b40e838b24cbcfbe07d78834b161e99ebef8562
stage 2   418e79f774a1ba89141463b536da3b47d3dc4bf1dcf29da69fc1ddc37b1b4b84
boot.sqfs 208db195aa31ddf9a10208cd75cf1cf60fca2580d2e3e0b939c13c66217b8b52
root.sqfs 0521950808de28396847da9757e61cd50ac06297821a2cad7af2e2523da315df
```

CSF self-signatures cover `CSF[0 : header.length]` (0x58 / 0x70 / 0x50 bytes) —
the command sequence only, not the appended key/signature blobs, which are
covered transitively by the certificate chain.

## Observations

Things worth recording, roughly in order of consequence:

1. **The env is unsigned, but it does not select the boot command.** HAB covers
   code, not the U-Boot environment — the env is raw at `mmcblk2 0x400000`,
   size `0x1000` (`/etc/fw_env.config`), writable from Linux with `fw_setenv`
   and from the U-Boot console, and nothing authenticates it beyond its CRC32.
   That matters less than it looks, because the autoboot command is a
   **compile-time literal in the signed U-Boot binary**, not `env_get("bootcmd")`
   — see [Boot command selection](#boot-command-selection). Rewriting `bootcmd`
   in the env has no effect on a normal boot.

   Three env variables do still reach boot policy, all requiring physical
   access:

   | Var | Effect | Reachability |
   | --- | --- | --- |
   | `bootdelay` | ≥ 0 re-enables the "Hit any key to stop autoboot" prompt; `-1` skips autoboot entirely. Either lands in `cli_loop()` — a U-Boot shell. | needs UART console |
   | `bootcmd_mfg` | run verbatim as the autoboot command when `is_boot_from_usb()` is true | needs USB/SDP boot |
   | `su_state` | steers `bootpp`'s A/B slot choice and rollback (both slots still HAB-checked) | slot selection only |

   So the residual exposure is the standard "secure boot ends where the
   interactive shell begins" problem, not a remote/software bypass. `booti`
   itself is gated and `hang()`s on a closed part, but `go` is not gated at all
   — see [Command-level gating](#command-level-gating). HAB still enforces SPL,
   U-Boot, ATF and OP-TEE, so the shell cannot replace the boot chain, only
   escape it for the kernel. Signing the environment (or `CONFIG_ENV_IS_NOWHERE`
   + a fixed `bootdelay`, plus dropping `go`/`bootelf` from the build) closes
   it.

2. **One key pair signs everything.** `CSF1_1`/`IMG1_1` cover SPL, U-Boot, ATF,
   OP-TEE, the kernel and the rootfs. Compromise of `IMG1_1` is total: it grants
   the ability to sign a rootfs *and* a bootloader, and the bootloader is
   field-updatable through the FOTA (`FILE.BOOT`). There is no separation
   between "signs the immutable boot chain" and "signs the frequently rebuilt
   rootfs".

3. **Stock CST PKI defaults.** Key CNs, the `0x12345678`-series serials and the
   `OpenSSL Generated Certificate` comment are unchanged from
   `hab4_pki_tree.sh`. Cosmetic, but it means the CA structure and key sizes are
   whatever the script's defaults were (RSA-2048/SHA-256, 4 SRKs), and there is
   no evidence of an HSM-backed or otherwise hardened issuance process.

4. **Signed but not encrypted.** No `HAB_PCL_BLOB`/AEAD commands, no DEK blob;
   the `dek_blob` command exists in U-Boot but is unused. All firmware is
   readable in the clear — which is why this analysis is possible at all.

5. **SRK rotation headroom exists but is unused.** Only SRK[0] is referenced;
   revoking it and re-signing against SRK[1] is possible on a closed part
   without replacing the SRK hash fuses.

6. **A failed authentication reboots rather than halts.** `bootpp` flips the
   eMMC boot partition and resets, so with both slots unbootable this is a reset
   loop rather than a console drop. If `bootpp` *returns* instead of resetting
   (the non-rollback failure path), `main_loop()` falls through to `cli_loop()`
   — a U-Boot prompt on the UART. With the default `bootdelay=-2` that is the
   only interactive entry point.

7. **`hab_auth_img_or_fail` fails *open* to the BootROM.** On authentication
   failure it prints `authentication fail -> %s %s %s %s` and calls
   `hab_rvt_failsafe()`, putting the SoC into USB serial-download mode. On a
   closed part SDP only accepts correctly signed images, so this is a recovery
   affordance rather than a hole — but it is exactly inverted on an open part.
   The command also refuses to run at all when `imx_hab_is_enabled()` is false
   (`error: secure boot disabled`), unlike the `hab_auth_*` family.

8. The HAB RVT is reached either by direct call through `*(0x918)` when running
   at EL3, or by `SMC 0xC2000007` (`IMX_SIP_HAB`) into ATF otherwise
   (`0x402034F8`, `0x4020355C`).

## Repro

```python
# CSF / SRK / cert extraction — offsets from the byte map above
import struct, hashlib
d = open('imx-boot_signed.bin','rb').read()
be16 = lambda b,o: (b[o]<<8)|b[o+1]

for name, ivt_off in (('stage1', 0x0), ('stage2', 0x59000)):
    hdr, entry, r1, dcd, bd, self_, csf, r2 = struct.unpack_from('<8I', d, ivt_off)
    C = csf - self_ + ivt_off                       # CSF file offset
    p, end = C+4, C+be16(d, C+1)
    while p < end:                                  # walk the command sequence
        tag, ln = d[p], be16(d, p+1)
        if tag == 0xBE:                             # INS_KEY: loc is CSF-relative (no ABS flag)
            loc = struct.unpack_from('>I', d, p+8)[0]
            blob = d[C+loc+4 : C+loc+be16(d, C+loc+1)]
            if d[C+loc] == 0xD7 and d[C+loc+3] == 0x40:   # SRK table (ver 0x40), not a CRT blob
                print(name, 'SRK hash', hashlib.sha256(d[C+loc:C+loc+be16(d,C+loc+1)]).hexdigest())
            else:
                open(f'{name}_key{d[p+7]}.der','wb').write(blob)
        p += ln
```

```sh
openssl x509 -inform DER -in stage1_key1.der -noout -text        # CSF cert
openssl cms  -inform DER -in <sig>.p7 -cmsout -print             # CMS signedAttrs
```

Squashfs images: the IVT sits at `(bytes_used + 0xFFF) & ~0xFFF`; the CSF at
IVT + `0x20`; the same walk applies.

Full signature verification needs the FIT `data-position` map for stage 2 (see
[Byte map](#byte-map)) — hashing the CSF block list by load address will not
reproduce `messageDigest`.
