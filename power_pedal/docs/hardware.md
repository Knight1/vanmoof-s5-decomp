# power_pedal — hardware notes

> The bike's **pedal-assist / torque + cadence sensor** sub-ECU. Reads pedal
> torque and crank cadence and publishes them as CAN object-dictionary signals;
> the assist curve itself is computed downstream by `motor_control`.

## Target

| Property | Value |
|---|---|
| MCU | NXP **LPC546xx** Cortex-M4F (VFPv4 single-precision, hard-float ABI) |
| Image | `power_pedal.20240129.145222.1.5.0.main.v1.5.0-main.bin` (28 496 bytes) |
| Image base | `0x0` (raw vector table) |
| Initial SP | `0x20008000` |
| Max address | `0x6f4f` |
| Build | `Jan 29 2024 14:50:32`, release `v1.5.0-main` (VMFW header @ `0x134`) |
| RTOS | FreeRTOS + ARM_CM4F port |
| Bus | Bosch M_CAN, 1 Mbps, 29-bit extended IDs |

## CAN identity

| Property | Value |
|---|---|
| PS address | `0x92` |
| PF=SA | `0xA2` |
| On-wire device-encoded | `(0xA2 & 0x7F) << 5 = 0x440` |
| Heartbeat ID | `0x01111000 + 0x440 = 0x01111440` |

See `../../canbus/CANBUS.md` for the full S5 CAN map.

## Memory map (observed globals)

| Address | Role |
|---|---|
| `0x20000924` | event-publisher context (ptr-to-ptr) — `PP_LOG_CTX` |
| `0x00003941` | message-buffer handle/tag for log records — `PP_LOG_TAG` |
| `0x40000000` | peripheral base used by `main_init` (M_CAN / SYSCON region; `+0xa18`, `+0x38c`, `+0x220`, `+0xd20`) |
| `0x40000100/120/140` | GPIO pin direction/set/clear (used by the HAL pin-wait helper `0x5af2`) |
| `0xe000ed04` | ARM SCB ICSR (PendSV trigger, vendor) |

## Notes

- power_pedal is a **sensor**, so — unlike the lights — it exposes **no**
  device-specific render/actuator function in the VanMoof application layer. Its
  reconstructable surface is the shared CAN-OD signal + event-log infrastructure
  (`can.c`). Torque/cadence acquisition is in the NXP ADC/HAL front-end and the
  monolithic init/task bodies (vendor / deferred).
- The OD opcodes `0x87` (secret), `0x91` (key), `0x8808` (signal) are identical
  to the elock / eshifter / light infrastructure.
