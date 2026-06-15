# Hardware — user_ecu (VanMoof S5 central ECU)

> Firmware: `user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin`. ARM
> Cortex-M4F, raw binary, image base `0x0`. Build stamp "Jan 29 2024 / 14:50:32"
> at `0x144` (matches filename). All addresses base-0 hex. TBC = to be confirmed.
> Findings below are from the `map-user-ecu` survey + 3-verifier pass.

## MCU identity (core CONFIRMED; family high; SKU unresolved)
- **Core: ARM Cortex-M4F (ARMv7-M)** with an active **single-precision VFPv4
  FPU**, hard-float ABI. CONFIRMED: `vPortStartFirstTask_ControlTask` (0x370)
  contains genuine VFP ops — `vcvt.f32.s32` @0x5d0, `vfma.f32` (fused MAC,
  VFPv4-specific) @0x600, `vcvt.s32.f32` @0x604, `vmul.f32` @0x618, `vdiv.f32`
  @0x64a, plus vsub/vmov/vldr.32/vstr.32. `vfma` rules out M0/M3.
- Implied build flags: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  -mthumb`.
- **Family: NXP-automotive SCG + FTFC + IFR-trim Cortex-M4F (S32K-family
  architecture)** — HIGH confidence (SIRC/FIRC/SOSC/SPLL clock generator, FTFC
  flash with factory IFR trim, PCC-style clock gating, 96 MHz from 12 MHz via
  SPLL).
- **Precise SKU = UNRESOLVED / LOW confidence. The S32K144 guess is REFUTED by
  the base map:** this firmware uses SCG@`0x40000000` and
  FTFC@`0x40020000`/`0x40034000`, whereas canonical S32K1xx has SCG@`0x40064000`,
  PCC@`0x40065000`, SIM@`0x40048000`. SCG offsets only partially match
  (FIRC@+0x300 matches; SOSC@+0x380 / SPLL@+0x580 do not match S32K1). Likely a
  related Kinetis/S32-adjacent or remapped part. (Also note the comms pass reads
  the data bus as an **Ambiq Apollo IOM** — in tension with NXP; one of the two
  vendor reads is imprecise. TBC via datasheet base-map match.)

## Memory map
| Region | Range | Notes |
|---|---|---|
| Code/Flash image | `0x00000000` .. `~0x0001a88b` | ~106 KB raw dump. Table at 0x0 is a relocated-from decoy (see Boot). |
| Data beyond image | `0x0001b2f2` .. `0x0001b364+` | xTaskCreate name strings (`0x1b2f2..0x1b341`) and CRC-8 LUT (`0x1b364`) live here — **absent from this raw dump**. TBC: load full ROM section. |
| SRAM | `0x20000000` .. `>= 0x20010000` | Initial SP `0x20010000`; observed data ptrs in `0x20000000..~0x20001Exx`. ~64 KB single region. TBC: full size. |

> On STM32/Kinetis parts flash is often at `0x08000000`/`0x0` and aliased; the
> all-`0x0000xxxx` vector targets are consistent with a base-0 (or aliased) part.

## Vector table (decoy at `0x0`; runtime table relocated)
- Read at `0x0` (verified): SP = **`0x20010000`**; Reset slot bytes `d5 0d 00 00`
  = **`0x00000dd4`** → code. `0xdd4` disassembles to `cmp.w r0,#0x8000` mid-Q16.16
  math; NMI `0xd98` / HardFault `0xd9c` likewise point into adjacent math. **These
  are NOT valid handlers — the table at `0x0` is not the runtime table.**
- Runtime VTOR is set to **`0x00000c00`** by `Reset_Handler`. CAVEAT: `0xc00`
  statically holds code (`31 42 c4 f8 ...`); no valid second Cortex-M vector table
  was found and no pointer to the `0x1d4` entry exists. Live table contents are
  **unresolved statically** (TBC: separate bootloader / runtime-relocated table).

## Reset / startup stub @`0x000001d4` (sole path to main; under-carved by Ghidra)
Verified instruction-by-instruction:
1. `cpsid i` — mask interrupts.
2. Load `0x00000c00`, write to **VTOR (`0xE000ED08`)** — relocate vector table.
3. Load `table[0]` → `msr msp`.
4. RMW **CPACR (`SCB+0x88`, SCB `0xE000ED00`)** `|= 0xF00000` then `|= 0xF` —
   enable FPU CP10/CP11.
5. `.data`-copy + `.bss`-zero loops (`0x228..0x274`). (Descriptor bounds
   `0xd5c/0xd80/0xd98` point at code, so loop detail is imperfect; boot conclusion
   stands.)
6. `bl main_SystemInit` (`0x44c0`) @`0x276`.
- Boot literal pool @`0x27c`: `0x00000c00`, `0x2000f000`, `0xE000ED00`,
  `0x00000d5c`, `0x00000d80`, `0x00000d98`, `0xE000ED08`.

## Cortex-M System Control Space (verified)
| Address | Block | Firmware use |
|---|---|---|
| `0xE000E000` | SysTick/SCS | CSR@+0x10, RVR@+0x14, CVR@+0x18 |
| `0xE000E100` | NVIC_ISER0 | per-driver IRQ enable (`periph_clk_nvic_enable` + 8 sites) |
| `0xE000ED00` | SCB | CPACR (+0x88) FPU enable; fault config |
| `0xE000ED08` | SCB_VTOR | vector relocation (write @startup, read @0x390) |
| `0xE000ED20` | SCB_SHPR3 | PendSV+SysTick lowest priority (`\|= 0xFFFF0000`) |

## Clock tree (verified constants)
- **Core clock = 96 MHz** from a **12 MHz external crystal via SPLL**. Also 1 MHz
  LPO and 32.768 kHz paths.
- Constants: `0x05B8D800` = 96,000,000; `0x00B71B00` = 12,000,000; `0x000F4240` =
  1,000,000 (LPO); `GetClock_32k` returns `0x8000` (32768).
- `SystemCoreClock` lives in **RAM @`0x20001654`**. Resolver
  `GetSystemCoreClockSource` branches on clock-select regs `0x40000280/0x40000284`
  and SCG status `0x40013010` bit14 / `0x40000a18` bit6. Full bring-up
  `SystemClock_PllFlashInit` enables SPLL, programs FTFC, applies factory trim from
  IFR @`0x3F000`.

## SysTick (RTOS tick) — CONFIRMED
At `0x6210..0x6248` in `main_SystemInit`: `SHPR3 |= 0xFFFF0000`; CSR=0; CVR=0;
**RVR = SystemCoreClock/1000 - 1 ⇒ 1 ms / 1 kHz**; CSR=7 (ENABLE|TICKINT|CLKSOURCE).

## On-chip peripheral map (absolute literals, verified)
| Base / offset | Inferred block | Evidence / use |
|---|---|---|
| `0x40000000` | SCG-like clock/reset mega-block (FIRC@+0x300, SOSC@+0x380/+0x388, SPLL@+0x580.., status@+0xa18, src-sel@+0x280/+0x284) | `main_SystemInit`, `SystemClock_PllFlashInit`, `GetSystemCoreClockSource` |
| `0x40004000` | per-channel clock-gate / port | `periph_clk_nvic_enable` |
| `0x40006000` | PCC-like clock control / GPIO pin-mux (+0xc0..+0x94) | `gpio_pin_config`, literal @0x4840 |
| `0x40013000` | clock/oscillator status | `GetSystemCoreClockSource`, `SystemClock_PllFlashInit` |
| `0x40020000` | FTFC flash controller (unlock/cmd @+0xc0/+0xc8) | `SystemClock_PllFlashInit` |
| `0x40034000` | FTFC flash (mirror; +0xfe8/+0x80/+0xfe0) | verifier |
| `0x40082000` | timer (FlexTimer/PWM-class) | literal @0x6034 |
| `0x4008C000` | timer/PWM MMIO base | literals @0x86c/0x880/0x1764/0x1de0/0x3fc8/0x6290 |
| `0x40086000` | GPIO (interrupt dispatch bank) | `gpio_bank_irq_trampoline_*` |
| `0x0003F000` | flash factory IFR / trim / UID (+0xce8/+0xcec/+0xd30/+0xd40) | `SystemClock_PllFlashInit` |

> CORRECTION (verifier, high confidence): **`0x40080000` is NOT MMIO.** It is the
> high word of the IEEE-754 double **3.0** (`0x4008000000000000`), loaded from the
> literal pool @`0x7cc` and consumed by soft-float `aeabi_ddiv_softfloat` (`0x8014`).
> `get_xrefs_to(0x40080000)` returns none. Likewise `0x3FDCCCCCCCCCCCCD` @`0x798`
> = double `0.45`. The earlier "peripheral access to 0x40080000" was a false positive.

## Subsystems
- **LED rings (L/R):** PWM timer with complementary outputs, programmed at
  `0x49a6..0x4b02` in `main_SystemInit` (handle from `DAT_00004944`). Period/duty
  = `LED_PWM_TIMER_CLK / ((hi+lo+3)*period)`, clamp `0x1ff`; complementary-output
  bits in reg +0x18; dead-time +0x48. Driven by Q16.16 `ledEasing_ControlUpdate`.
- **Sensors:** (a) on-chip ADC (`Adc_ReadCh_LPO1MHz`, `GetClock_32k`,
  `GetSystemCoreClockSource`, `*5.75 / *1.5` scaling ⇒ voltage/current); (b)
  I²C/SMBus-PEC sensor bus (see `protocol.md`); `sensor_read_sht_temp_humidity` =
  Sensirion-style temp/humidity; companion-MCU position/speed via
  `controlTask_CmdHandler`.
- **DMIC (microphone):** SAI/I²S + DMA decimation path (handle `iRam00005cb4`),
  configured `~0x5b00..0x5f3c`: clock/decimation divider search writing
  base+0x800/0x810/0x814/0x824; DMA at +0xc00/+0xc04/+0xc1c and +0xe00/+0xe08.
  `dmic_task` prio 4 (highest named). Sample/RMS logic in the not-yet-defined task
  body. TBC.

## SRAM globals found (verified / proposed)
| Address | Name | Role |
|---|---|---|
| `0x20001654` | `g_SystemCoreClock` | core-clock value; SysTick RVR = this/1000 |
| `0x200007f4` | `g_iom_bus_handle_ptr` | shared I²C/IOM HAL handle pointer |
| `0x200007f8` | `g_iom_bus_handle_ptr2` | second bus handle slot |
| `0x20001ce4` | `g_ctrl_struct_A` | Q16.16 easing state, channel A (left ring) |
| `0x20000860` | `g_ctrl_struct_B` | Q16.16 easing state, channel B (right ring) |
| `0x200016a0`/`0x200016c4` | gpio_irq arg/handler tables | GPIO IRQ dispatch parallel tables |

## Open hardware items (TBC)
- Exact NXP SKU (base map refutes S32K144; family is S32K-like). Reconcile vs the
  Ambiq-IOM comms read.
- Live vector-table contents at VTOR=`0xc00` and how the CPU first enters `0x1d4`
  (likely a separate bootloader).
- Identity of timers `0x40082000` vs `0x4008C000` (FTM/LPIT/PDB).
- Whether the `0x40000000`+`0x38c` read-loop-with-float-scale is TRNG/entropy vs
  ADC/temp-trim.
- Exact sensor part(s) on the I²C bus (addressed numerically; no WHO_AM_I string).
