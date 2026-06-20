# eshifter — hardware / image facts

> The VanMoof S5/A5 **electronic gear shifter** (automatic transmission
> actuator). Raw ARM Cortex-M image, base `0x0`. CAN node **`0x91`** / PFSA
> `0xC2` (on-wire device-encoded `0x840`).

## Image

| | |
|---|---|
| File | `eshifter.20240129.145222.1.5.0.main.v1.5.0-main.bin` |
| Size | 29676 bytes (`0x73ec`) |
| Header | shared VanMoof **VMFW** header @ `0x134` (build `Jan 29 2024 14:50:32`) |
| Layout | raw Cortex-M vector table @ `0x0`; image base `0x0`; SP `0x2000_8000` |
| Reset | `0x00000d9c` (Thumb), NMI `0x00000d84`, HardFault `0x00002648` |

## MCU — NXP LPC546xx (Cortex-M4F)

Same family as elock / power_control / motor_sensor. Build flags:
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`. 32 KB SRAM @ `0x2000_0000`.

| Peripheral | Where | Use |
|---|---|---|
| **SCTimer/PWM** | `0x4008_5000` region (set up in `main_init`) | gear-actuator motor drive |
| **Bosch M_CAN** | driver block (CCCR `+0xff8`, mode `+0x800`, timing `+0x810`, IE `+0x80c`) | bike CAN bus (1 Mbps, 29-bit IDs) |
| **Flexcomm** | `+0x10` cfg, `+0xf0/+0xf4/+0xf8/+0xfc` | actuator-position sensor (quadrature/SPI), wheel counter |
| **ADC** | via SDK helper | rail/current sense |
| **SYSCON / PMU** | `0x4000_0000` region (`+0x148/+0x220/+0x224/+0x228/+0x300..+0x400/+0xa18`) | clocks, variant select, peripheral power |
| **SCB / NVIC** | `0xE000_Exxx` | FreeRTOS port |

## Software stack

- **FreeRTOS** (ARM_CM4F port): heap_4, queues/stream-buffers, software timers,
  critical sections; task-create helper `FUN_00003014`. `main_init`
  (`FUN_00003f98`) creates **3 tasks** + registers the eshifter CAN-OD signals.
- **NXP SDK/HAL**: M_CAN, SCTimer/PWM, Flexcomm, ADC, flash, clock drivers.
- A **CAN Object-Dictionary comms-registry middleware** (register `FUN_00005eac`,
  lookup `FUN_000059c8/059e0`, wait `FUN_00005f10`, release `FUN_0000656e`,
  multi-frame dispatch) — same generic layer as elock / power_control.
- `FUN_0000332c` is the shared VanMoof **event-record emitter** (same shape as
  elock `elock_log_event` / power_control `power_log_event`).

## What it does (application layer)

The **automatic e-shifter**: actuates the internal gear hub between gears on
command. On-wire protocol is fully mapped in `vanmoof/canbus` (`eshifter.go`,
`CANBUS.md`):

- **Gear command** `0x18411840` (`01 01 mode gear 00`; mode 0=set, 1=force).
- **Config / sensor feed** `0x10F11840` (`00 A9 FC 8C type …`: enable 0x55, set
  gear 0x44, config 0x66, speed 0x8D/0x98, clear 0xCA, confirm 0xEE/0xEF).
- **Telemetry** `0x10F11841` (cadence + actuator position).
- **Status report** `0x1840B110` (status flags, current gear, params) + ack
  `0x1840B100`.
- **Heartbeat** `0x01111840`.

The gear-shift FSM + actuator drive live in the task bodies spawned by
`main_init` (`FUN_00006852`, `FUN_00006d14`, and the thunk `FUN_0000476c`).

> The application code was hidden in an undisassembled indirect-dispatch gap
> (`0x3f62`–`0x599b`, ~6.7 KB); recovered by flow-disassembly + function creation
> (106 → 146 functions). See `progress.md`.
