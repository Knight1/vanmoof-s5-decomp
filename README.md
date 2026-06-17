# vanmoof-s5-decomp

Clean-room rebuild of the **VanMoof S5 / A5** firmware (internal codename
`XS5`) from decompilations, producing buildable source trees that re-emit
binary-equivalent (or behaviour-equivalent) images.

Sibling project to [`vanmoof-s3-decomp`](https://github.com/Knight1/vanmoof-s3-decomp). The S5 is a
generation newer and architecturally very different: instead of a handful of
small MCUs on a serial bus, it runs a **Linux application processor** (i.MX8
class), a **cellular modem** (nRF9160), a **BLE SoC** (nRF5x, Zephyr), and a
fleet of **Cortex-M sub-ECUs** on a CAN bus. This repo decomposes that stack
target by target.

> Status: **first target active.** The repo documents the firmware layout and
> each target's container format / MCU. **[`user_ecu/`](user_ecu/)** is the
> first target under reconstruction: **~121 VanMoof functions translated to C**
> (all compile clean for Cortex-M4F), the FreeRTOS/libgcc/SDK code identified and
> deferred as vendor, and the static frontier exhausted. The boot/clocks, I²C +
> CAN comms, FOTA/storage, device registry, the handlebar buttons, and the LED
> control chain are documented under [`user_ecu/docs/`](user_ecu/docs/). The
> remaining per-target subdirectories are created as work on each begins.

## Firmware source

The reference image is the **`v1.5.0-main`** FOTA bundle (build `20240129`,
version `1.5.0`), located at:

```
../VanMooof-Firmware/SA5/
├── v1.5.0-main/
│   └── v1.5.0-main                ← gzip → tar → SquashFS rootfs (the i.MX8 Linux system, ~57 MB)
└── v1.5.0-main_device_files/      ← the individual per-ECU images + manifest.txt
```

`manifest.txt` is the authoritative list of components shipped in this release
(`Filename,Device,Date,Time,Major,Minor,Patch,Type,AllowSkip,DontRollback`).
Rendered readable (all `main` images share build `2024-01-29 14:52:22` except
`motor_control` at `15:42:39`; the battery/charger blobs are vendor `untagged`):

| Device | Version | Type | Build | Skippable | No-rollback |
| --- | --- | --- | --- | --- | --- |
| `imx8_bridge` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `ble` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `modem` | 1.5.0 | main | 2024-01-29 14:52:22 | **yes** | no |
| `motor_sensor` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `elock` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `user_ecu` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `frontlight` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `rearlight` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `eshifter` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `motor_control` | 1.5.0 | main | 2024-01-29 15:42:39 | no | no |
| `power_control` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `power_pedal` | 1.5.0 | main | 2024-01-29 14:52:22 | no | no |
| `battery_primary_panasonic` | 1.4.256 | untagged | — | **yes** | **yes** |
| `charger_liteon_normal` | 1.8.0 | untagged | — | **yes** | **yes** |
| `charger_liteon_speed` | 1.2.0 | untagged | — | **yes** | **yes** |

> `AllowSkip` = the updater may skip this image; `DontRollback` = it must not be
> rolled back to an earlier version (set on the vendor battery/charger blobs).

## Targets

Container formats and sizes below are confirmed from the image headers; MCU
part numbers marked *(tbc)* are inferred from the vector layout / SDK and are
to be confirmed in Ghidra.

### Application processor (Linux)

| Component | Image | Format | Size | Status |
| --- | --- | --- | --- | --- |
| `main` (i.MX8 system) | `v1.5.0-main` → `VM-XS5_FOTA` | gzip → tar → **SquashFS** (zlib) | ~57 MB | pending — unpack rootfs, enumerate services |

### Wireless SoCs (Zephyr + MCUboot, magic `0x96f3b83d`)

| Component | Image | MCU | Size | Status |
| --- | --- | --- | --- | --- |
| `ble` | `ble.*.bin` | Nordic nRF5x *(tbc)*, Zephyr | ~273 KB | pending |
| `modem` | `modem.*.bin` | Nordic **nRF9160** (LTE-M/NB-IoT + GNSS), Zephyr | ~302 KB | pending |

Both carry an MCUboot image header (`0x96f3b83d`); the payload is a Zephyr/nRF
Connect SDK application. `modem` also ships a vendor modem firmware
(`mfw_nrf9160_1.3.1.zip`) in the device-files directory.

### Sub-ECUs (ARM Cortex-M, raw vector table)

All start with a Cortex-M vector table (initial SP `0x2000_8000`, except
`user_ecu` at `0x2001_0000`). Part numbers *(tbc)* via Ghidra.

| Component | Image | Role | Size | Status |
| --- | --- | --- | --- | --- |
| [`user_ecu`](user_ecu/) | `user_ecu.*.bin` | handlebar controller — **Cortex-M4F**, **FreeRTOS**; handlebar buttons + cluster LEDs, sensors, USB-C phone-charge monitor; I²C+CRC-8 + CAN comms | ~106 KB | **active** — 192 fns analyzed, **~121 reconstructed to C**; boot/comms/control/FOTA/buttons/LED documented |
| `imx8_bridge` | `imx8_bridge.*.bin` | gateway between the i.MX8 and the ECU/CAN bus | ~24 KB | pending |
| `motor_control` | `motor_control.*.bin` | motor controller (non-standard header `0x000008aa`) | ~25 KB | pending |
| `motor_sensor` | `motor_sensor.*.bin` | motor position/torque sensing | ~25 KB | pending |
| `power_control` | `power_control.*.bin` | power management | ~29 KB | pending |
| `power_pedal` | `power_pedal.*.bin` | pedal-assist sensing | ~28 KB | pending |
| `elock` | `elock.*.bin` | electronic frame lock | ~27 KB | pending |
| `eshifter` | `eshifter.*.bin` | e-shifter (auto gearbox) | ~29 KB | pending |
| `frontlight` | `frontlight.*.bin` | front light | ~28 KB | pending |
| `rearlight` | `rearlight.*.bin` | rear light | ~27 KB | pending |

#### `user_ecu` documentation

The first reconstructed target is documented under
[`user_ecu/docs/`](user_ecu/docs/):

| Doc | Covers |
| --- | --- |
| [`architecture.md`](user_ecu/docs/architecture.md) | what the firmware does — boot, RTOS startup, tasks, control/math, the two comms paths, subsystems, reconstruction status |
| [`hardware.md`](user_ecu/docs/hardware.md) | MCU identity, memory/peripheral map, clock tree, SRAM globals, the USB-C phone-charge power monitor |
| [`protocol.md`](user_ecu/docs/protocol.md) | the I²C wire framing + CRC-8, **and the CAN/device command dispatch** (inbound command frames, device-record commands, outbound framing, event/status records, the comm-port frame path) |
| [`buttons.md`](user_ecu/docs/buttons.md) | the handlebar buttons — GPIO scan, debounce, and **exactly what CAN data each press/release sends** |
| [`led_control.md`](user_ecu/docs/led_control.md) | the LED control chain end-to-end with CAN/I²C worked examples |
| [`progress.md`](user_ecu/docs/progress.md) | per-function tracker (source of truth for what's left) |

> **CAN bus:** the on-MCU command/event framing is documented in `protocol.md`
> and `buttons.md`; the **on-wire CAN ID/DLC mapping is off-image** (it lives in
> the application processor's upper protocol layer, not in any sub-ECU image).

### Peripheral / vendor firmware

| Component | Image | Vendor | Size | Status |
| --- | --- | --- | --- | --- |
| `battery_primary_panasonic` | `battery_primary_panasonic.0.0.1.4.256.*.bin` | Panasonic BMS — encrypted (a `_DECRYPTED.bin` is present); header `"(C) 2021 Energy Company of Panasonic Group"` | ~98 KB | pending |
| `charger_liteon_normal` | `charger_liteon_normal.0.0.1.8.0.*.bin` | LiteON charger (normal) | ~23 KB | pending |
| `charger_liteon_speed` | `charger_liteon_speed.0.0.1.2.0.*.bin` | LiteON charger (speed) | ~23 KB | pending |

## Planned repository layout

Mirrors `vanmoof-s3-decomp`: one self-contained subdirectory per target, each
created as work begins.

```
vanmoof-s5-decomp/
├── README.md
├── .gitignore
├── Makefile                ← top-level dispatcher (added with the first buildable target)
├── main/                   ← i.MX8 Linux rootfs analysis (SquashFS unpack, service map)
├── ble/                    ← nRF5x BLE app (Zephyr/MCUboot)
├── modem/                  ← nRF9160 cellular app (Zephyr/MCUboot)
├── user_ecu/               ← main vehicle controller
├── imx8_bridge/            ← i.MX8 ↔ ECU bus gateway
├── motor_control/          ├ motor_sensor/
├── power_control/          ├ power_pedal/
├── elock/  eshifter/  frontlight/  rearlight/
├── battery/                ← Panasonic BMS (encrypted)
├── charger/                ← LiteON chargers
├── tools/                  ← cross-target tooling (FOTA unpack, MCUboot/CAN helpers)
└── reference/              ← shared datasheets, CAN/protocol notes, pin maps
```

Each target subdirectory follows the S3 convention: `src/`, `include/`,
`docs/` (`progress.md`, `hardware.md`, `protocol.md`), `ghidra/exports/`, and a
linker script + `Makefile`.

## Legal

This is a clean-room interoperability project. OEM firmware images are **not**
redistributed in this repository — extract them from a FOTA bundle or flash
dump of a bike you own.

Reverse engineering for interoperability is permitted under EU Software
Directive 2009/24/EC Art. 6 and US DMCA §1201(f). The reconstructed source in
this repository is original work derived from analysis of the OEM image and
publicly available documentation.

No warranty. Flashing reconstructed firmware to a bike will **brick or damage
hardware** if it is wrong. Do not flash to a bike you depend on. Use a spare
PCB or hardware-in-the-loop simulator.
