# elock — hardware / image facts

> The VanMoof S5/A5 **electronic frame / kick-lock controller**. Raw ARM Cortex-M
> image, base `0x0`. CAN node **`0xC1`** (on-wire device-encoded `0x820`).

## Image

| | |
|---|---|
| File | `elock.20240129.145222.1.5.0.main.v1.5.0-main.bin` |
| Size | 27952 bytes (`0x6d30`) |
| Header | shared VanMoof **VMFW** header @ `0x134` (build `Jan 29 2024 14:50:32`) |
| Layout | raw Cortex-M vector table @ `0x0`; image base `0x0`; SP `0x2000_8000` |

## MCU — NXP LPC546xx (Cortex-M4F)

Confirmed against the bus captures (`vanmoof/canbus` `CANBUS.md`). Build flags:
`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`. 32 KB SRAM @ `0x2000_0000`.

| Peripheral | Where | Use |
|---|---|---|
| **SCTimer/PWM** | `0x4008_5000` | lock-actuator **motor** drive (duty = pct·period/100; MATCHREL +0x100, reload +0x200; direction bit) — `lock_motor_pwm_set_duty` |
| **Bosch M_CAN** | driver block (bit-timing/ID filters set in `main_init`) | the bike CAN bus (1 Mbps, 29-bit IDs); TX driver `FUN_000035c0 → FUN_00002140` |
| **Flexcomm I2C** | `+0x800` CFG/MSTCTL, `+0x804` STAT, `+0x820/+0x828` MST data | the **accelerometer** (anti-theft tilt sensing) |
| **ADC** | via `FUN_00000504` + FP scale `×5.75` | rail voltage/current sense |
| **SYSCON / PMU** | `0x4000_0000` region (`+0x048/+0x380/+0x400/+0x704`, `+0xFFC` rev, `+0x280/+0x284` variant selectors) | clocks, motor power-rail enable, HW-variant select |
| **SCB / NVIC** | `0xE000_ED04` ICSR (PENDSV), `0xE000_ED0C` AIRCR (reset) | FreeRTOS port |
| **NVM / flash** | device-proxy + page driver | persisted 16-byte **AES key** + 14-byte **secret** + config |

## Software stack

- **FreeRTOS** (ARM_CM4F port): heap_4, queues/stream-buffers, timers, lists,
  critical sections (BASEPRI), `configASSERT` trap; task name `sensor_task`
  (@`0x6cc0`). `main_init` (`0x396a`) creates **~5 tasks**.
- **NXP SDK/HAL**: M_CAN, SCTimer/PWM, ADC, Flexcomm I2C, flash, clock drivers.
- A **CAN Object-Dictionary comms-registry middleware** (register / lookup / wait
  / release / multi-frame dispatch) — the same generic layer seen in
  power_control (`comms_registry_lookup`).
- **AES** block-cipher primitive (vendor). The elock encrypts a CAN data layer;
  the **key slot is selected by lock state** (per `CANBUS.md`: slots 1/2/4/8 ↔
  UNKNOWN/LOCKED/UNLOCKED/STUCK). elock manages the 16-byte AES key + 14-byte
  secret in NVM (`elock_load/store_aeskey_91` / `..secret_87`).

## What it does (application layer)

The **kick/frame lock**: `elock_lock_task` runs the lock state machine
(UNKNOWN/LOCKED/UNLOCKED/STUCK, broadcast on CAN OD `0x4c1` / on-wire
`0x18209820`), driving the lock **motor** open/closed; it listens for the unlock
trigger (`0x1800D110#01`) and reports state. It also runs the **anti-theft
alarm**: an accelerometer (I2C) feeds an IIR tilt filter (threshold 7.0, 8-sample
debounce) that fires an alarm and commands `motor_sensor`/`power_pedal`. It holds
an **AES-encrypted secure channel** keyed by lock state. Everything else
(FreeRTOS, the NXP HAL, the OD middleware, the AES primitive, and the SDK-heavy
`main_init` bring-up) is vendor, deferred.

> The application code was originally hidden in an undisassembled indirect-dispatch
> gap (`0x3949`–`0x5588`); recovered by flow-disassembly + function creation
> (85 → 131 functions). See `progress.md`.
