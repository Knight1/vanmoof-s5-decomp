# motor_update_lib — TI C2000 motor-controller SCI flasher (i.MX8)

`/usr/bin/motor_update_example` — the VanMoof S5 standalone **TI C2000
(TMS320F280049C) motor-controller firmware flasher**, package
`vmxs5-embedded-motor-update-lib`. Stripped AArch64 C++ ELF
(`AARCH64:LE:64:v8A`, base `0x100000`, ~471 functions), Ghidra
`/S5-v1.5/OS/motor_update_example`.

The package also installs **`/usr/bin/tms320f40049c_sci_kernel.bin`** — the TI
SCI flash-kernel blob uploaded in stage 1 (a TI artifact, **vendor / deferred**).

## What it does

Flashes the motor controller over its **SCI serial bootloader** in two stages:

1. **Boot + kernel.** Drive the boot-mode GPIO into SCI-boot, autobaud-lock the
   DSP **ROM bootloader** (send `'A'`, wait for the echo), then **byte-echo
   upload** the SCI flash kernel into DSP RAM (every byte sent is echoed back and
   verified).
2. **Program.** Autobaud-lock the now-running flash kernel and stream the
   **application** image to it with a **DFU block protocol** — a 10-byte start
   packet, checksummed data blocks (running 16-bit sum, verified every 256 bytes
   and at each block boundary), `{u16 count, u32 addr}` block headers, terminated
   by a zero-count block — then read the status (`0x1be4` start header, status
   `0x1000` = OK, `0xe41b` completion). Finally restore the boot-mode GPIO to run.

```
Usage: motor_update_example [-g] [-p <ttydev>] <kernel file to send> <app file to send>
  -b   skip setting boot mode (handy on a devboard)
  -p   ttydev (default /dev/ttymxc3)
```
(The usage text prints `[-g]` but the implemented flag is `-b` — an OEM doc bug.)
Exit `0` on `Flashing application... Done!`; `-1` on a caught failure
(`exception caught: <msg>` — e.g. `baud rate detection failed`, `Invalid
checksum`, `Flashing failed`); `-22` on a usage error.

## Scope

The VanMoof-authored code is the CLI ([`src/main.c`](src/main.c)) and the flasher
library — boot-mode GPIO, autobaud, kernel upload, DFU app programmer
([`src/c2000_flash.c`](src/c2000_flash.c)). The serial transport
(`serial_port.cpp` termios), sysfs GPIO (`gpio.cpp`), `std::ifstream` and
libstdc++ are vendor — modelled over POSIX in `c2000_flash.c` so the protocol
logic is self-contained; the TI SCI wire format is reproduced verbatim. The
`tms320f40049c_sci_kernel.bin` blob is not reconstructed.

- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/motor_update_example_program.json`](ghidra/exports/motor_update_example_program.json)

## Build

```sh
make            # src/{main,c2000_flash}.c -> build/*.o, clean -Wall -Wextra -Wpedantic
```
