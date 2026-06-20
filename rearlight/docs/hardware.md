# rearlight — hardware / image facts

> The VanMoof S5/A5 **rear LED light controller**. Raw ARM Cortex-M image,
> base `0x0`. CAN node **`0x88`** / PFSA `0xC3` / on-wire device-encoded `0x860`.
> Same firmware family as the **frontlight** (node `0x87` / `0x880`) — recompiled
> from the same source, so the application functions are structurally identical
> at shifted addresses, with node byte `0x88` instead of `0x87`.

## Image

| | |
|---|---|
| File | `rearlight.20240129.145222.1.5.0.main.v1.5.0-main.bin` |
| Size | 27744 bytes (`0x6c60`) |
| Header | shared VanMoof **VMFW** header @ `0x134` (build `Jan 29 2024 14:50:32`) |
| Layout | raw Cortex-M vector table @ `0x0`; image base `0x0`; SP `0x2000_8000` |
| Reset | `0x00000dc0` (Thumb) — same as frontlight |

## MCU — NXP LPC546xx (Cortex-M4F)

Same family as frontlight / elock / eshifter. Build flags:
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`. 32 KB SRAM @ `0x2000_0000`.

| Peripheral | Use |
|---|---|
| **SCTimer/PWM** (`0x4008_5000`) | LED **brightness** drive (duty from a 0–100% value via gamma/dimming curve) |
| **Bosch M_CAN** | bike CAN bus (1 Mbps, 29-bit IDs) |
| **flash/NVM** | persisted config + gamma/brightness lookup tables |

A large **data/table region `0x59b6`–`0x6c5f`** (~4.8 KB, no code) at the end of
the image — brightness/gamma lookup curves.

## On-wire protocol (from `vanmoof/canbus` `frontlight.go` / `CANBUS.md`)

| Role | CAN ID | Payload |
|---|---|---|
| Heartbeat | `0x01111860` | 8 bytes |
| Command **to** rearlight | `0x18627860` | `{brightness 0–100, mode/0x01}` |
| Init handshake | `0x10F11860` | — |
| Brightness status **from** | `0x18605110` | `{brightness 0–100}` (+ ack `0x18605100`) |
| Feedback **from** | `0x1860F110` | (+ ack `0x1860F100`) |
| Detailed status **from** | `0x18623110` | `{…, brightness@3, mode, devparam}` |

## What it does (application layer)

Drives the rear LED at a commanded **brightness (0–100%)** via PWM, runs the
animation/render path (gamma LUT + master brightness), reports brightness
status/feedback on CAN, and participates in light-pair sync with the frontlight.
FreeRTOS, the NXP HAL, the OD middleware and the SDK-heavy `main_init` are vendor.

> The application code was partly hidden in an undisassembled indirect-dispatch
> gap (`0x340a`–`0x4835`, ~5.2 KB); recovered by flow-disassembly + function
> creation. See `progress.md`.
