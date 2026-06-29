# `motor_control` — BLDC motor-drive controller (TI C2000 / C28x)

The bike's **motor-drive controller**: the real-time DSP that runs the
field-oriented control (FOC) loop — PWM commutation, phase-current sensing and
the motor command/status handling — for the VanMoof S5/A5 mid-drive. It is a
**Texas Instruments C2000 (TMS320F280049C, C28x core)**, *not* an ARM Cortex-M
like the other sub-ECUs, which is why it ships in a different image format and a
different toolchain build.

> Status: **identified + image structure decoded; not yet disassembled.** The
> binary is a **TI C28x boot-table** (boot key `0x08AA`). Ghidra in this project
> has no **TMS320C28x** processor module (the import fails with *"Language not
> found"*), so function-level decompilation is **blocked here** — see *Tooling*.
> What is established below (ISA, boot/load map, role, relationships) comes from
> parsing the image and the surrounding firmware.

## Image

`opt/devices_fw/motor_control.20240129.154239.1.5.0.main.v1.5.0-main.bin`
- 25,890 bytes = **12,945 16-bit words** (C28x is word-addressed).
- Build timestamp in the filename is **15:42:39** — distinct from the `14:50:22`
  GCC-ARM build time shared by every other in-house ECU, consistent with a
  separate **TI Code Composer Studio** build.
- **No `VMFW` header** (offset `0x134` is code, not the magic). The image is a
  raw TI boot-table, header word `0x08AA`.

## Boot-table / flash load map (parsed)

The TI C28x serial-boot table format: a 16-bit key, 8 reserved words, a 2-word
entry point, then `{size, dest_hi, dest_lo, data[size]}` blocks terminated by a
zero size. Decoded:

| Field | Value |
|---|---|
| Boot key | `0x08AA` (16-bit SCI/flash boot data stream) |
| Reserved words [1..8] | all `0x0000` |
| **Entry point** | **`0x080000`** (FLASH start — the C28x reset/`_c_int00` path) |

| Segment | Dest (word addr) | Size | Region |
|---|---|---|---|
| 0 | `0x080000` | 2 words | FLASH — entry branch |
| 1 | `0x080004` | 1622 words (3244 B) | FLASH — C-runtime init + startup |
| 2 | **`0x08065C`** | **11137 words (22274 B)** | FLASH — the application (FOC + CAN + control) |
| 3 | `0x0831E0` | 160 words (320 B) | FLASH — const/data |

All four segments land in on-chip **flash** (`0x080000`–`0x0831E0`), as expected
for a flash-resident motor-control image. Total 12,921 words loaded (the whole
file, clean zero terminator).

## Role & relationships

- **Drives the motor.** As the C2000 in a VanMoof drivetrain, it runs the
  current/position control loop and PWM; the [`motor_sensor`](../motor_sensor/)
  board supplies rotor-position / current / temperature feedback, and
  `motor_control` consumes it (over CAN and/or a direct link) to close the loop.
- **CAN node.** It exchanges motor command/status frames with the fleet — see
  `vanmoof/canbus` `CANBUS.md` for the motor-controller CAN role.
- **Flashed over SCI** by the standalone **`motor_update_lib`** tool
  (`../main/motor_update_lib/`), which is exactly a TMS320F280049C SCI bootloader
  (boot-GPIO → autobaud → kernel upload → DFU) — independent confirmation of the
  part number.

## Tooling — disassembled with IDA 7.0 (C28x), pipeline reproducible

Stock Ghidra has no TMS320C28x SLEIGH, but **IDA 7.0** (`/mnt/c/Program Files/
IDA 7.0/`) ships the native **`tms32028`** processor — the same disassembler the
sibling S3 `motorware` was decoded with, on this machine. The pipeline is wired
up and reproducible:

```
make image   # tools/bootstream.py -> build/image/region_*.bin + manifest.json
             #   (raw LE, 2 bytes per C28x word, named by load word-address)
make ida     # ida/build_db.py via idat64 -ptms32028 (headless, TVHEADLESS=1):
             #   rebases regions, defines RAM/peripheral segments, sets the
             #   0x080000 entry, force-codes the flash regions, and exports
             #   build/ida/functions.asm + funcmap.json + mmio_hits.json
```

### Result

The `make ida` pass disassembles the application: **87 functions / ~8934 C28x
instructions** (`build/ida/functions.asm`). The boot entry `0x080000` is
`lb codestart_0` (the C-runtime start); the call graph has the expected motor-
control shape — a couple of large init functions and heavily-shared leaf helpers
(e.g. the 44-caller `EALLOW`-guarded register-bit helper `sub_82BA5`). Direct
MMIO xrefs show PIE-vector setup + GSx-RAM state; the EPWM/ADC/eQEP/CAN control
is **XARn-pointer-based** (the S3 caveat) and needs pointer-setup tracing to
attribute.

IDA gives **disassembly only** (no Hex-Rays for C28x), so the C is hand-written
from `functions.asm` — the same ASM→C step as the S3 motorware (`foc.c`,
`comm.c`, …). That reconstruction is the next pass.

## Layout

```
motor_control/
  README.md           — this file (ISA, boot/load map, role, tooling)
  Makefile            — analysis-only (info / image / ida)
  tools/bootstream.py — C28x boot-table parser + flash-region extractor
  ida/build_db.py     — IDA tms32028 disassembly driver (F280049C-rebased)
  src/                — 87 functions reconstructed to reference C (NOT compiled)
  include/            — motor_control.h (C2000Ware-style register model)
  build/image/        — extracted flash regions + manifest (regenerated)
  build/ida/          — functions.asm + funcmap.json + clusters (regenerated)
  docs/hardware.md    — TMS320F280049C / C28x detail
  docs/progress.md    — per-function status + reconstruction map
  docs/function_map.md — all 87 functions: addr / name / subsystem / role
```
