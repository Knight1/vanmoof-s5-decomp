# user_ecu — Architecture Overview

`user_ecu` is the central controller ECU of the VanMoof S5 e-bike: an ARM
**Cortex-M4F** (ARMv7-M, VFPv4 single-precision FPU, hard-float ABI) firmware
running a **FreeRTOS** Cortex-M4F port. The image is a raw binary, base `0x0`,
~106 KB code (`0x0..~0x1a88b`), SRAM at `0x20000000` (≥64 KB, initial SP
`0x20010000`). Logging strings are stripped; only RTOS task names survive.

## Boot
The vector table at offset `0x0` is a **decoy / relocated-from table**: the SP
(`0x20010000`) is valid, but the Reset slot literally holds `0x00000dd4`, which
lands mid-`ledEasing_ControlUpdate` (`cmp.w r0,#0x8000`); NMI/HardFault likewise
point into adjacent math. The **real reset / C-runtime startup is the stub at
`0x000001d4`** (Ghidra never auto-defined it): mask IRQs, write
`VTOR (0xE000ED08) = 0x00000c00`, set MSP from the relocated `table[0]`, enable
the FPU (`CPACR` at `SCB+0x88 |= 0xF00000, 0xF`), run `.data`-copy / `.bss`-zero,
then `bl main_SystemInit` (`0x44c0`) at `0x276`. How the CPU first reaches `0x1d4`
and the true contents of the `0xc00` table are **unresolved statically** (`0xc00`
holds code here — likely a separate first-stage bootloader not in this dump, or a
runtime-built table).

## main / RTOS startup
`main_SystemInit` (`0x44c0`) is `main` / system init (**not** a command
dispatcher, as first-pass recon guessed): brings up clocks
(`SystemClock_PllFlashInit` — SPLL/flash/IFR-trim), GPIO pin-mux, peripheral
clock-gates; configures the LED-PWM / ADC / I²C-IOM / SAI-DMIC blocks inline;
spawns 8 FreeRTOS threads via `xTaskCreate` (`0x3144`); sets `SHPR3` (PendSV +
SysTick lowest priority) and a **1 ms SysTick** (`RVR = SystemCoreClock/1000 - 1`,
`CSR = 7`); then calls `vPortStartFirstTask` (`0x370`). FreeRTOS is confirmed by
BASEPRI critical sections (`vPortRaiseBASEPRI` sets `BASEPRI = 0x20` =
`configMAX_SYSCALL_INTERRUPT_PRIORITY`) and the standard manufactured exception
frame (`xPSR 0x01000000`, `EXC_RETURN 0xFFFFFFFD`).

## Tasks
Eight tasks are created; four are named (`l_led_ring_task`, `r_led_ring_task`,
`sensors_task`, `dmic_task`) plus IDLE and helpers. The name-pointers passed to
`xTaskCreate` point at `0x1b2f2..0x1b341`, **past the image end (`0x1a88b`)** —
that data section is absent from this raw dump, so the name→entry binding is read
from literal pools and is **medium confidence**: `l_led_ring=0x473c`,
`r_led_ring=0x4768` (stack `0x186`/prio 3), `sensors=0x0f98` (stack `0xb4`/prio
1), `dmic=0x4be8` (stack `0x5a`/prio 4).

## Control / math
The control/easing cluster is **signed Q16.16** (`1.0 == 0x00010000`) — NOT Q22
(the multiply `q16_mul` rounds with `+0x8000` then `>>16`, verified).
`ledEasing_ControlUpdate` (`0xc64..0xd95`) is a per-tick scalar easing kernel
(startup ramp, rate-limited slew, logistic/exp shaping via `q16_exp`/`q16_sigmoid`,
triple adaptive EMA, damped second-order integrator) — most consistent with
**LED-ring brightness/animation easing**, not motor torque (FOC lives in the
separate `motor_control` ECU). `controlTask_CmdHandler` (`0x1ee0`) reads scaled
sensor counts from a companion MCU over a checksummed packet and runs the kernel
on two channel structs (`0x20001ce4` / `0x20000860`, the L/R rings).

## Comms
Single bus: **I²C master via an Ambiq Apollo-class IOM** (I/O Master). Engine
`iom_i2c_transfer` (`0x7288`) programs the IOM command window and blocks on an RTOS
mutex/semaphore (synchronous; no RX ring buffer). Framing = big-endian 16-bit
words, **each followed by a CRC-8 byte** (poly `0x31`, first byte inverted, no
final XOR). Opcodes select transaction type (`0x53/0x153`, `0x44/0x144`,
`0x59/0x159`; bit `0x100` = read). Peers are 8-bit I²C addresses. See
`protocol.md`. (NB: the IOM points to Ambiq silicon, which is in tension with the
S32K-like clock/flash block seen by the hardware pass — see `hardware.md` open
items.)

## Subsystems
LED rings (PWM timer with complementary outputs, Q16.16-eased), sensors (on-chip
ADC + I²C/SMBus-PEC sensor bus incl. a Sensirion temp/humidity part), and a
digital microphone (SAI/I²S + DMA decimation). GPIO interrupts are dispatched by a
separate bank (`gpio_irq_dispatch` + `irqn_to_gpio_index` + the `0x2100..0x22b0`
trampolines), unrelated to the data bus.
