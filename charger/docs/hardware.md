# charger — hardware notes

> The S5's **Liteon charger** (model `5EL00000000EB`). Negotiates and delivers
> the battery charge over CAN; lives on a **separate CAN bus segment** from the
> main bike bus, talking to the battery BMS (`charger_target` node `0x8D`).

## Target

| Property | Value |
|---|---|
| Vendor | Liteon (third-party) |
| MCU | ARM Cortex-M (Thumb-2 capable, **soft-float** — no VFP in the float-format code) |
| Image | `charger_liteon_normal.0.0.1.8.0.untagged.x.bin` (23 340 B) |
| Image base | `0x0` (raw vector table) |
| Initial SP | `0x20008000` |
| Variants | `normal` (v0.0.1.8.0), `speed`/fast-charge (v0.0.1.2.0) — see `variants.md` |
| CAN | Bosch **M_CAN** controller (registers at `0x4009xxxx`), node `0x70` |

## Firmware makeup

- **newlib/libc** — printf, `fopen`, `strlen`, `memcpy`, 64-bit divmod, soft-float.
- **Bosch M_CAN driver + register HAL** — Tx/Rx message-RAM, ID filters, accessors.
- **Cooperative mini-RTOS / event core** — `FUN_00001074` post primitive, timers.
- **Flash self-programming** — `FUN_00001e64` page-program loop (FOTA/identity).
- **Charger application** — an ~11-state `tbb` charge state machine, temperature-
  derated current/voltage setpoint policy, CAN report/command handlers, identity
  and fault management. This is the layer reconstructed in `src/`.

## Observed globals

| Address | Role |
|---|---|
| `0x20000a94` | charge-setpoint output struct (HW DAC/PWM fields at +4 / +6) |
| `0x20000aac` | u16 software status / fault bit-field |
| `0x20000ac2` | charger fault-state global |
| `0x4009xxxx` | Bosch M_CAN registers |
| `0x40000000 +0x120/0x140/0x220` | GPIO/clock bring-up |

## CAN

The charger is node `0x70` on the charger bus segment; `A5 5A` is the fleet's
charger command/unlock magic (see `vanmoof-canbus/CANBUS.md`, charger node `0xA7` /
`0x70`).
