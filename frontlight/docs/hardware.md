# frontlight — hardware / image facts

> The VanMoof S5/A5 **front LED light controller**. Raw ARM Cortex-M image,
> base `0x0`. CAN node **`0x87`** / PFSA `0xC4` / on-wire device-encoded `0x880`.
> The **rearlight** (node `0x88` / `0xC3` / `0x860`) is the same firmware family.

## Image

| | |
|---|---|
| File | `frontlight.20240129.145222.1.5.0.main.v1.5.0-main.bin` |
| Size | 28900 bytes (`0x70e4`) |
| Header | shared VanMoof **VMFW** header @ `0x134` (build `Jan 29 2024 14:50:32`) |
| Layout | raw Cortex-M vector table @ `0x0`; image base `0x0`; SP `0x2000_8000` |
| Reset | `0x00000dc0` (Thumb), NMI `0x00000d84`, HardFault `0x00000d88` |

## MCU — NXP LPC546xx (Cortex-M4F)

Same family as elock / eshifter / power_control. Build flags:
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`. 32 KB SRAM @ `0x2000_0000`.

| Peripheral | Use |
|---|---|
| **SCTimer/PWM** | LED **brightness** drive (duty from a 0–100% brightness value, likely via a gamma/dimming curve) |
| **Bosch M_CAN** | bike CAN bus (1 Mbps, 29-bit IDs) |
| **ADC / Flexcomm** | rail/LED current sense, possibly ambient light |
| **SYSCON / PMU** | clocks, LED rail power |
| **flash/NVM** | persisted config |

A large **data/table region `0x5ece`–`0x70e3`** (~4.6 KB, no code) sits at the end
of the image — candidate brightness/gamma lookup curves.

## Software stack

- **FreeRTOS** (ARM_CM4F port) + **NXP SDK/HAL** + the generic **CAN
  Object-Dictionary comms-registry middleware** (register / lookup / wait /
  release / multi-frame dispatch) shared across the sub-ECUs.
- `main_init` (`FUN_000034dc`, ~4.9 KB) = SDK peripheral bring-up + FreeRTOS task
  creation + CAN-OD signal registration (deferred-as-vendor).

## On-wire protocol (from `vanmoof/canbus` `frontlight.go` / `CANBUS.md`)

| Role | CAN ID | Payload |
|---|---|---|
| Heartbeat | `0x01111880` | 8 bytes |
| Command **to** frontlight | `0x18827880` | `{brightness 0–100, mode/0x01}` |
| Init handshake | `0x10F11880` | — |
| Brightness status **from** | `0x18805110` | `{brightness 0–100}` (+ ack `0x18805100`) |
| Feedback **from** | `0x1880F110` | (+ ack `0x1880F100`) |
| Detailed status **from** | `0x18823110` | `{…, brightness@3, mode, devparam}` |

## What it does (application layer)

Drives the front LED at a commanded **brightness (0–100%)** via PWM, runs the
light on/off/auto state, reports brightness status/feedback on CAN, and
participates in **light-pair sync** with the rearlight. The brightness→duty
mapping (gamma/dimming) and the CAN command/status handlers are the VanMoof
application layer; FreeRTOS, the NXP HAL, the OD middleware and the SDK-heavy
`main_init` are vendor.

> The application code was partly hidden in an undisassembled indirect-dispatch
> gap (`0x34c6`–`0x4a2b`, ~5.5 KB); recovered by flow-disassembly + function
> creation. See `progress.md`.
