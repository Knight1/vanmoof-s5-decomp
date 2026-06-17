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

**Task-entry anomaly (investigated):** these `pxTaskCode` entries do **not** point
at real prologues — they land in the *interior* of existing functions (`0x473d`/
`0x4769` inside `main_SystemInit`'s NVIC sweep; `0x0f99` inside the control/easing
region). The real task bodies are most likely **off-image**; the entries are not
separately carvable. Do not re-carve them. See `progress.md` worklist item 1.

## Control / math
The control/easing cluster is **signed Q16.16** (`1.0 == 0x00010000`) — NOT Q22
(the multiply `q16_mul` rounds with `+0x8000` then `>>16`, verified).
`ledEasing_ControlUpdate` (`0xc64..0xd95`) is a per-tick scalar easing kernel
(startup ramp, rate-limited slew, logistic/exp shaping via `q16_exp`/`q16_sigmoid`,
triple adaptive EMA, damped second-order integrator) — most consistent with
**LED-ring brightness/animation easing**, not motor torque (FOC lives in the
separate `motor_control` ECU). `controlTask_CmdHandler` (`0x1ee0`) reads scaled
sensor counts from a companion MCU over a checksummed packet and runs the kernel
on two channel structs (`0x20001ce4` / `0x20000860`, the L/R rings). The control
task also uses a small VFP helper `int_pair_to_float` (`0x884`) to fold a
`{whole, micros}` integer pair into a single-precision float (`p[0] + p[1]/1e6`).

## Comms
Two independent comm paths:

**(1) I²C master via an Ambiq Apollo-class IOM** (I/O Master). Engine
`iom_i2c_transfer` (`0x7288`) programs the IOM command window and blocks on an RTOS
mutex/semaphore (synchronous; no RX ring buffer). Framing = big-endian 16-bit
words, **each followed by a CRC-8 byte** (poly `0x31`, first byte inverted, no
final XOR). Opcodes select transaction type (`0x53/0x153`, `0x44/0x144`,
`0x59/0x159`; bit `0x100` = read). Peers are 8-bit I²C addresses. See
`protocol.md`. (NB: the IOM points to Ambiq silicon, which is in tension with the
S32K-like clock/flash block seen by the hardware pass — see `hardware.md` open
items.)

**(2) CAN/CAN-FD comm port** (`0x40089000`, instance 3 of a 4-member family). There
is **no classic FlexCAN mailbox driver**: the MCU-side data path is a **software
descriptor ring + eDMA** (`0x40082000`). TX `commport_can_transmit` builds eDMA
TCDs and kicks the FIFO; the ISR (`commport_irq_dispatch_inst3` → FIFO body →
`commport_rx_complete_cb`) drains RX via task-notify. Inbound frames are decoded to
`{dlc, 29-bit id, payload}` and dispatched through the device manager
(`device_dispatch_command`); a 30-byte event/status queue at `devmgr+0x590` feeds a
comms task that transmits over CAN. The on-wire CAN ID/DLC mapping lives in the
off-image upper protocol layer.

## Subsystems
- **LED rings** — PWM timer with complementary outputs, Q16.16-eased
  (`ledEasing_ControlUpdate`).
- **Sensors** — on-chip ADC + I²C/SMBus-PEC sensor bus incl. a Sensirion
  temp/humidity part.
- **Digital microphone** — SAI/I²S + DMA decimation.
- **GPIO interrupts** — dispatched by a separate bank (`gpio_irq_dispatch` +
  `irqn_to_gpio_index` + the `0x2100..0x22b0` trampolines), plus a clock-gate IRQ
  trampoline family that acks status via `clockgate_status_ack` — unrelated to the
  data bus.
- **External storage + FOTA updater** — off-chip flash holds a staged firmware
  image + 8-byte descriptor (111 KB data region in `0x200`-byte pages). A `bus_*`
  vtable dispatch layer with a `store_*` page-cache reads/writes/verifies it
  (HW checksum engine at `0x40095000`); the actual image swap is done by the
  off-image bootloader.
- **Device registry + manager** — fixed `0x2c`-byte registry slots keyed by a
  3-byte tag cache device records; `device_*` accessors transfer records over the
  bus and `device_dispatch_command` routes inbound CAN commands (single-shot /
  streamed / multi-fragment reassembly).
- **Event / notify** — stream-buffer event posters (`event_report` + the `event.c`
  family) push status/error records to the manager's `+0x590` queue; a GPIO
  button-scan/debounce poller feeds input events.

## Reconstruction status
The **VanMoof static frontier is exhausted** (batch 27): every function reachable
in this image is reconstructed (~121 VanMoof functions in C), named vendor
(FreeRTOS kernel/port, libgcc soft-float, CMSIS — `deferred`, never reconstructed),
or flagged do-not-reconstruct. Per-function fidelity is verified by `objdump`
against the OEM (the image is not linkable here — no startup/FreeRTOS — so
`make compare` cannot run). Remaining VanMoof recovery requires the **off-image
ROM** (`0x13xxxxxx` handlers, the `VTOR=0xc00` table, the `>0x1a88b` data/name
region) or dynamic analysis — not more static carving. See `progress.md` for the
per-function tracker.
