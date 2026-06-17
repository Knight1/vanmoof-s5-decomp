# user_ecu — decompilation progress

Per-function tracker. Source of truth for "what's left to do."

> Companion docs: **`architecture.md`** (overview), **`hardware.md`** (memory
> map, MCU, peripherals, SRAM globals), **`protocol.md`** (I²C wire framing,
> command dispatch & device-level commands) and **`led_control.md`** (the LED-ring
> control chain end-to-end + CAN/I²C worked examples). Renamed functions are
> exported to `ghidra/exports/user_ecu_program.json`.

Status legend:
- **pending** — not started (still `FUN_*` in Ghidra)
- **named** — renamed in Ghidra, role identified, no C yet
- **decomp-c** — translated to C, builds, but bytes diverge from OEM
- **byte-eq** — translated to C and `make compare` reports zero diff
- **deferred** — intentionally skipped (library/dead code)

_Last refresh: 2026-06-17 — batch 27 (final `FUN_*` frontier sweep). Classified all
12 remaining `FUN_*` + the one named-but-no-C function. Translated the **2 genuine
VanMoof** leaves (`int_pair_to_float` 0x884 → control.c, `clockgate_status_ack`
0x84be → pcc.c — both verified instruction-byte-exact by objdump). The other 10
`FUN_*` are FreeRTOS vendor: renamed the 4 freshly-confirmed ones (`xQueueReceive`,
`prvAddCurrentTaskToDelayedList`, `prvTimerTask`, `prvTaskExitError`); 5 were already
documented vendor; 2 (`0x9408` stream-buffer callback pump, `0x9544` bad-data trap)
are **flagged do-not-reconstruct**. **The VanMoof static frontier is now exhausted.**
All compile clean._

_(Prior: batch 26 — orphaned-gap carve + translate, 16 functions across the new
`event.c` plus store/comm/device; `vmem_copy`→`void *`, `i2c_reg_write_53`→`int`.)_

## Summary

| Status | Count |
| --- | --- |
| pending (still `FUN_*`, VanMoof) |   0 |
| named (no C yet / extern) |   5 |
| decomp-c (VanMoof)| 119 |
| byte-eq           |   8 |
| deferred (vendor) |  66 |
| flagged (do-not-reconstruct) |   2 |

_Counts re-synced to the Ghidra export `ghidra/exports/user_ecu_program.json`
(machine-recounted from the 192 Ghidra functions, batch 27): **119** reconstructed
to C, **5** named-but-no-C (`Reset_Handler` ×2, the `sensors_task` interior label,
`iom_i2c_transfer` and `xfer_waiter_reset` left extern), **66** vendor (61
named + 5 still-`FUN_*`), **2** flagged. The earlier hand-tally undercounted the
FreeRTOS functions Ghidra had named across batches; the export is now authoritative._

_**VanMoof static frontier exhausted (batch 27).** 7 `FUN_*` remain in Ghidra — all
FreeRTOS vendor (5: `0x15e0` xTimerCreate, `0x6dc4` task-notify wait, `0x6e20`
queue-receive internal, `0x6fd0` timer-cmd dispatch, `0x74e0` task-notify give) or
flagged do-not-reconstruct (2: `0x9408`, `0x9544`). None are VanMoof application
code. The image is not linkable (no startup/FreeRTOS), so fidelity is verified
**per function** by a relocation-masked byte compare against the OEM image (see
*Validation* below): **8 of 119** are byte-identical; the rest are
behaviour-oriented (encoding differs)._

## Validation — per-function byte compare vs OEM (batch 27)

Because the image cannot be linked here, each reconstructed function is verified
directly against the OEM bytes: extract its `.text.<name>` from the `-Os` `.o`
(`objcopy`), zero the relocation sites (bl targets + ABS32 literal-pool addresses,
which only resolve at link time; from `objdump -r`), slice the OEM image at the
function's address for the same length, mask the same offsets, and compare. Equal
masked bytes ⇒ **byte-identical modulo link-time addresses**.

**Result (119 decomp-c functions):**

| Bucket | Count | Meaning |
| --- | --- | --- |
| **byte-identical** | **8** | `bus_variant_b`, `commport_base_to_index`, `registry_find`, `q16_sigmoid`, `clockgate_status_ack`, `registry_add`, `mem_free`, `event_handler_dispatch` |
| close (<15% bytes differ) | 2 | `xfer_waiter_notify` (10%), `gpio_base_to_bank` (11%) — encoding-only |
| medium (15–50%) | 28 | same logic, different reg-alloc/scheduling |
| far (>50%) | 81 | substantially different encoding |

**Interpretation — this is a behaviour-oriented reconstruction, NOT (yet) a
byte-matching decomp.** The C reproduces the OEM *logic* (translated from the
disassembly/decompiler and logic-checked during each batch), but is not tuned to
emit the OEM's exact instruction stream. An opt-level sweep confirms `-Os` is the
best fit (`-O1` 1, `-O2`/`-O3` 2, **`-Os` 8** exact), so optimisation level is not
the blocker. The dominant cause is the **toolchain**: ours is **GCC 9.2.1
(2019-q4)** vs an OEM image built **2024-01-29** (almost certainly GCC 12/13.x);
different GCC majors emit very different code at the same `-O`.

**Path to a byte-identical / provably behaviour-identical build (large, optional):**
- *Matching decomp* — pin the exact OEM `arm-none-eabi-gcc` (2023-era 12.x/13.x),
  then iterate each function's C until the masked compare is clean. Gold standard;
  this is typically the bulk of a matching-decomp project.
- *Emulation-based equivalence* — run OEM vs reconstructed each function on sample
  inputs (Ghidra `emulate_function` / QEMU) and compare results + side effects.
  Proves behaviour-equivalence without byte-matching.

The harness is reproducible (objcopy/objdump/objdump -r + the OEM image); rerun it
after any toolchain change.

_Core CONFIRMED: **ARM Cortex-M4F** (VFPv4, hard-float). RTOS CONFIRMED:
**FreeRTOS**. **Batch 1**: 14 functions translated to C, all compile clean
(`arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -Wall
-Wextra -Wpedantic`, zero warnings). Per the **VanMoof-code-only** decomp-scope
rule, the 4 FreeRTOS port functions in `port.c` are
**vendor** and reclassified `deferred` (satisfied by upstream FreeRTOS, not
reconstructed here) — 10 are real VanMoof code. Vendor functions named in Ghidra
for understanding but not reconstructed: `xTaskCreate`, `vTaskSwitchContext_SelectNext`,
`aeabi_ddiv_softfloat` (libgcc)._

## Decompiled — batch 1 (`src/`, `include/`)

Translate→verify workflow; all module verifiers returned **faithful**. Each `-c`
compiles with zero warnings. Not yet linked (needs startup + the rest of the image).

| Module | File | Functions | Note |
| --- | --- | --- | --- |
| q16 | `src/q16.c`, `include/q16.h` | `q16_mul`, `q16_div`, `q16_sqrt` | solid |
| q16 | (same) | `q16_exp`, `q16_sigmoid` | ⚠️ **table-unverified** — algorithm+clamps faithful, but the factor tables at `0xa4e0`/`0xa4f0` in this dump are anomalous (not valid exp multipliers; likely the same image/region issue as the boot anomaly). Numerics not trustworthy until the real table is recovered. |
| crc8 | `src/crc8.c`, `include/crc8.h` | `crc8_poly31_word` | solid |
| crc8 | (same) | `crc8_poly31_verify_lut` | LUT `crc8_poly31_lut[256]` @`0x1b364` is **off-image** — declared `extern`, must be supplied/regenerated before linking. |
| frame | `src/protocol.c`, `include/protocol.h` | `frame_append_word_crc` | solid (calls `crc8_poly31_word`) |
| ~~port~~ **VENDOR, REMOVED** | ~~`src/port.c`~~ deleted | `vPortRaiseBASEPRI`, `vPortSetBASEPRI`, `vPortEnterCritical`, `vPortExitCritical` | **FreeRTOS port — `deferred` (vendor).** `port.c`/`port.h` **deleted** (not VanMoof code); the 4 primitives stay named in Ghidra and are satisfied by upstream FreeRTOS at link time. |
| hal | `src/hal.c`, `include/hal.h` | `GetClock_32k` | VanMoof (borderline — SCG access may be SDK-derived) |
| hal | (same) | `irqn_to_gpio_index` | VanMoof glue; ⚠️ **table-unverified** — table at `0xa510` (same anomalous region) doesn't match caller keys. |

**Classification (VanMoof-code-only rule):** `q16.*` (custom Q16.16 app math),
`crc8.*` + `protocol.c` (VanMoof wire protocol), and `hal.c` (clock/GPIO glue,
borderline) are **real VanMoof code → keep**. `port.c` is **FreeRTOS → vendor →
deferred**. The libgcc soft-float helper `aeabi_ddiv_softfloat` and the FreeRTOS
`xTaskCreate` / `vTaskSwitchContext_SelectNext` are vendor (named only, never to
be reconstructed).

## Decompiled — batch 2 (`src/`, `include/`)

VanMoof comms + control. Translate→verify workflow (i2c & control **faithful**;
sensor **minor-issues**, the one issue fixed below). All compile clean
(`-Wall -Wextra -Wpedantic`, zero warnings). Vendor deps left `extern`.

| Module | File | Functions | Note |
| --- | --- | --- | --- |
| i2c | `src/i2c.c`, `include/i2c.h` | `i2c_reg_write_53`, `i2c_reg_read_153`, `i2c_tx_frame`, `i2c_rx_frame_verify`, `i2c_control_write`, `i2c_read_status_e28` | VanMoof I²C transaction/opcode layer over the IOM. `iom_i2c_transfer`, `vmemset_00009866`, `rtos_delay_00001bdc` left `extern` (vendor). `i2c_control_write` corrected to return `int` (OEM tail-returns status). |
| sensor | `src/sensor.c`, `include/sensor.h` | `sensor_read_sht_temp_humidity` | Sensirion SHT/SHTC read + conversion (T·0x5573, RH·0x3d09, >>13). Uses the off-image `crc8_poly31_lut` (extern). |
| control | `src/control.c`, `include/control.h` | `ledParams_Init`, `ledEasing_ControlUpdate`, `controlTask_CmdHandler` | LED-ring control. `ledEasing_ControlUpdate` reconstructed across the decoy-vector split (full body `0xc64..0x1032`) with a typed `LedCtrlState` struct. `q16_exp`/`q16_sigmoid` table caveat applies to its numerics only. |

**Vendor `extern` (not reconstructed):** `iom_i2c_transfer` (Ambiq IOM),
`vmemset_00009866`, `rtos_delay_00001bdc` / `busyWait_Ticks` (FreeRTOS/SDK).
SRAM globals (`g_iom_ctx_*`, `g_ctrl_struct_A/B` @0x20001ce4/0x20000860,
`g_sensor_*`) declared `extern` — defined by startup/linker later.

**Cross-module reconciliation TODO (latent — does not block `-c`):** `control.c`
declares `i2c_read_status_e28` returning `int` and `i2c_tx_frame`/`i2c_rx_frame_verify`
taking `void *`, while `i2c.h` has `void` / `uint8_t *`. Each TU compiles via its
own decls; reconcile the signatures when the modules are linked (verify
`i2c_read_status_e28`'s true return at `0x8964`). `i2c_reg_read_153` does not
zero its descriptor in the OEM (we do, for defined C).

## Open questions

1. ~~Reset-vector anomaly~~ **RESOLVED (high).** The table at `0x0` is a decoy;
   the real reset stub is `Reset_Handler` @`0x1d4` (sets `VTOR=0xc00`, MSP, enables
   FPU, `.data`/`.bss`, `bl main_SystemInit`). Remaining: how the CPU first reaches
   `0x1d4` and the live table at `VTOR=0xc00` (likely a first-stage bootloader not
   in this dump). See `architecture.md` / `hardware.md`.
2. **MCU SKU — still open.** Family is HIGH-confidence NXP **S32K-like** (SCG/FTFC/
   PCC/IFR-trim, 96 MHz from 12 MHz SPLL), but **S32K144 is REFUTED** by the base
   map (`SCG@0x40000000` vs canonical `0x40064000`). Also unreconciled with the
   comms pass's "Ambiq Apollo IOM" read. Confirm via datasheet base-map match.
3. ~~FPU?~~ **RESOLVED.** Cortex-M4F with active VFPv4 (`vfma.f32` etc.); the
   control path is **Q16.16** (`1.0 == 0x00010000`), NOT Q22 (first-pass recon was
   wrong). Build: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`.
4. **Off-image data.** xTaskCreate name strings (`0x1b2f2..`) and the CRC-8 LUT
   (`0x1b364`) sit past the image end (`0x1a88b`) and are absent from this dump —
   load the full ROM section to confirm the task name→entry binding.

## Named functions

| Address | Name | Role | Confidence |
|---|---|---|---|
| `0x000001d4` | `Reset_Handler` | True reset/C-runtime startup (VTOR=0xc00, FPU, .data/.bss, bl main). Body under-carved (16 B). | high |
| `0x000044c0` | `main_SystemInit` | C entry / sysinit: clocks, GPIO mux, periph bringup, 8× xTaskCreate, SHPR3 + 1 ms SysTick, bl 0x370 | high |
| `0x00000370` | `vPortStartFirstTask_ControlTask` | FreeRTOS start-first-task (svc #2) + control-task body + comms orchestrator | high |
| `0x00003144` | `xTaskCreate` | FreeRTOS task create | high |
| `0x000063c0` | `vTaskSwitchContext_SelectNext` | Scheduler dispatch (per-priority ready queue) | high |
| `0x0000950a` | `vPortRaiseBASEPRI` | Enter critical: BASEPRI=0x20 | high |
| `0x00009520` | `vPortSetBASEPRI` | Exit critical: restore BASEPRI | high |
| `0x00006454` | `vPortEnterCritical` | Lock: raise BASEPRI + nest++ | high |
| `0x00006470` | `vPortExitCritical` | Unlock: nest-- ; if 0 restore | high |
| `0x00001244` | `SystemClock_PllFlashInit` | SPLL/FTFC/IFR-trim bringup; publishes SystemCoreClock | high |
| `0x000011c0` | `GetSystemCoreClockSource` | Resolve active clock freq (12/96/1 MHz) | high |
| `0x00001170` | `Adc_ReadCh_LPO1MHz` | ADC read / 1 MHz LPO const | high |
| `0x00001188` | `GetClock_32k` | Returns 0x8000 (32.768 kHz) | medium |
| `0x00000c64` | `ledEasing_ControlUpdate` | Per-tick Q16.16 easing kernel (body 0xc64..0xd95) | high |
| `0x0000835e` | `q16_mul` | Signed Q16.16 saturating multiply | high |
| `0x000083b8` | `q16_div` | Signed Q16.16 division | high |
| `0x0000841e` | `q16_sqrt` | Q16.16 base-4 square root | high |
| `0x00000b04` | `q16_exp` | Q16.16 exp(), table-driven | high |
| `0x0000847c` | `q16_sigmoid` | Q16.16 logistic | high |
| `0x00000b60` | `ledParams_Init` | Init ~0xa0-byte Q16.16 control struct (×2 channels) | high |
| `0x00001ee0` | `controlTask_CmdHandler` | Cmds 0x1c/0x1d/0x3a: read sensor counts, run easing on L/R structs | high |
| `0x00007288` | `iom_i2c_transfer` | Core I²C/IOM transfer engine (mutex-guarded blocking) | high |
| `0x00007408` | `i2c_tx_frame` | TX submit, opcode 0x59 | high |
| `0x000073a0` | `i2c_rx_frame_verify` | RX opcode 0x159; deframe + per-word CRC verify | high |
| `0x00007364` | `i2c_control_write` | Control write, opcode 0x44 | high |
| `0x0000899c` | `frame_append_word_crc` | Append BE word + CRC-8 byte | high |
| `0x0000955e` | `crc8_poly31_word` | CRC-8 poly 0x31, first byte inverted (bit-banged) | high |
| `0x00006564` | `crc8_poly31_verify_lut` | Table-driven CRC-8/PEC (LUT off-image) | high |
| `0x00001a34` | `i2c_reg_write_53` | Register write, opcode 0x53 | high |
| `0x00001a70` | `i2c_reg_read_153` | Register read, opcode 0x153 | high |
| `0x00001e1c` | `sensor_read_sht_temp_humidity` | Sensirion SHT/SHTC temp/humidity read | high |
| `0x00008964` | `i2c_read_status_e28` | Status read, header 0xe28 | high |
| `0x00008014` | `aeabi_ddiv_softfloat` | Soft-float double divide (proves 0x40080000 = const 3.0) | high |
| `0x00007988` | `periph_clk_nvic_enable` | Peripheral clock-gate + NVIC enable | high |
| `0x000067f4` | `nvic_clockgate_bringup` | Disable+clear IRQ 33 (NVIC ICER1/ICPR1) around a clock-gate-5 enable (DSB/ISB) | high |
| `0x000065a0` | `gpio_pin_config` | GPIO set/clear/direction (bit-band) | high |
| `0x00002018` | `gpio_irq_dispatch` | GPIO ISR: walk handler table | high |
| `0x000020e4` | `irqn_to_gpio_index` | Map IRQn → 0..8 table index | high |
| `0x00002100` | `gpio_bank_irq_trampoline_0` | Per-GPIO-bank IRQ trampoline (1 of 10) | high |
| `0x00003698` | `rtos_sem_take` | RTOS semaphore/mutex take | medium |
| `0x00006ec0` | `rtos_sem_give` | RTOS semaphore give | medium |
| `0x0000473c` | `l_led_ring_task` | Left LED-ring task entry (label; in main body, re-carve) | medium |
| `0x00004768` | `r_led_ring_task` | Right LED-ring task entry (label; re-carve) | medium |
| `0x00000f98` | `sensors_task` | Sensor polling task entry | medium |
| `0x00004be8` | `dmic_task` | Digital-mic task entry (label; re-carve) | medium |
| `0x0000110c` | `clock_div_program` | Packed clock-divider programmer (`0x40020000`/`0x40000260`) | high |
| `0x000084b2` | `busy_wait` | Counted spin-delay loop | high |
| `0x0000984c` | `vmem_copy` | Hand-written forward byte copy (`memcpy`-equiv) | high |
| `0x000082c6` | `registry_find` | Comparator linear scan over 0x2c-byte slots | high |
| `0x00008594` | `registry_add` | Dedup + append 0x2c-byte entry to a registry | high |
| `0x000082f2` | `registry_lookup` | `registry_find` → slot pointer or NULL | high |
| `0x0000830a` | `registry_lookup_value` | Look up by inline 2-word key → slot ptr | high |
| `0x00008d54` | `xQueueSemaphoreTake` | FreeRTOS semaphore take — **vendor (deferred)** | medium |
| `0x0000982c` | `vmem_cmp` | Hand-written byte compare (`memcmp`-equiv); libc trio w/ copy/set | high |
| `0x00008e76` | `device_read_record87` | Read 14-byte record (sub-addr 0x30), cache under key {id,0x87}, opt. verify | high |
| `0x00008fd6` | `device_read_record91` | Read 16-byte record (sub-addr 0x40), cache under key {id,0x91}, opt. verify | high |
| `0x00008e0a` | `device_store_field8c0` | Write 3-byte field into device {0xc0,0x08} + apply/notify hook | high |
| `0x00008f76` | `device_cmd_read87` | Command write+verify, commit, then `device_read_record87` | high |
| `0x000097f4` | `rtos_sem_give_dispatch` | FreeRTOS semaphore give (ISR/thread dispatch) — **vendor (deferred)** | medium |
| `0x000087f2` | `mem_free` | NULL-guarded `vPortFree` wrapper (firmware's generic free) | high |
| `0x00006b9c` | `vPortFree` | FreeRTOS heap_4 free — **vendor (deferred)** | high |
| `0x0000288c` | `bus_variant_b` | Board/bus selector (`mgr+6 == 3` → variant-B vtable) | high |
| `0x00002910` | `bus_transfer` | Read/xfer dispatch (driver vtable +0x2c/+0x38) | high |
| `0x00006538` | `bus_write_commit` | Write dispatch (driver vtable +0x24/+0x30) | high |
| `0x000065dc` | `bus_probe_read` | 0x200-page probe-read dispatch (vtable +0x40/+0x4c) | high |
| `0x0000289c` | `bus_session_init` | Session init + timing recompute (`0xa0000 − 17·param`) | high |
| `0x000089c0` | `bus_mode_autodetect` | Latch the higher-metric of modes 1/2 | high |
| `0x000028c8` | `bus_session_open` | Alloc(0x3c) + init + driver open + autodetect | high |
| `0x00002938` | `bus_session_commit` | 0x200-page read-modify-write (set/clear flag+sentinel) | high |
| `0x00008b12` | `bus_page_write_verify` | Splice into 0x200 page → write-back → read-back `vmem_cmp` | high |
| `0x0000664c` | `bus_transfer_token` | Variant-dispatched 0x200 op + fixed token (vtable +0x08 / 0x1300413a) | medium |
| `0x00006610` | `bus_page_program` | Variant-dispatched 0x200-byte page write (vtable +0x0c / 0x1300419c) | medium |
| `0x00008876` | `store_flush` | Storage page-cache: write-back cached page + advance/invalidate | high |
| `0x000088d2` | `store_load` | Storage page-cache: flush + prefetch a page range into the window | high |
| `0x000064f0` | `checksum_feed` | Stream bytes into the HW checksum/CRC engine (`0x40095000`+8) | high |
| `0x000029b4` | `bus_page_read` | Variant-dispatched page read into a buffer (3 off-image handlers) | high |
| `0x00003eac` | `event_report` | Post a variadic event/error record to the manager's queue | high |
| `0x00003d80` | `xfer_state_lock_post` | Mark an xfer-state lock held (state=2, busy guard), run teardown cb, post header-only control record to `mgr+0x590` | high |
| `0x0000442c` | `flash_page_write` | Program a 0x200 page + read-back verify + error report | high |
| `0x0000668c` | `flash_page_commit` | Stage scratch page + program descriptor + token | high |
| `0x00002acc` | `fota_image_verify` | Checksum the staged image region (HW engine `0x40095000`) | high |
| `0x00006708` | `store_descriptor_read` | Read the 8-byte image descriptor @ `0x37400` | high |
| `0x00006794` | `store_descriptor_write` | Write the 8-byte image descriptor | high |
| `0x0000681c` | `log_append_event` | Append an 8-byte event record via the descriptor writer | high |
| `0x0000926c` | `xStreamBufferSend` | FreeRTOS stream-buffer send (stream_buffer.c) — **vendor** | high |
| `0x00002c20` | `commport_base_to_index` | Comm-port base → instance index 0..3 (family `0x40086000`) | high |
| `0x000024f4` | `commport_registry_index` | 8-slot comm-port key registry linear search | high |
| `0x00008a7a` | `edma_tcd_build` | eDMA Transfer Control Descriptor builder (4 words) | high |
| `0x00008c46` | `edma_chan_irq_enable` | Set the eDMA channel interrupt-enable bit | high |
| `0x00002754` | `commport_uart_config` | LPUART line/baud/FIFO config of a comm-port instance | high |
| `0x000022b0` | `peripheral_clock_mux_select` | PCC clock gate + functional-source select (`+0xff8`) | high |
| `0x00007550` | `xStreamBufferReceive` | FreeRTOS stream-buffer receive — **vendor** (was mis-recon'd VanMoof; C removed) | high |
| `0x000091ec` | `prvReadBytesFromBuffer` | FreeRTOS stream-buffer dequeue copy — **vendor** (C removed) | high |
| `0x000079c0` | `commport_teardown` | Disable/teardown the comm-port instance @ `0x4009d000` | high |
| `0x00002510` | `commport_fifo_isr` | FIFO PIO service / completion (carved via create_function) | high |
| `0x00002190` | `commport_irq_dispatch_inst3` | Vector IRQ trampoline → callback table (inst 3) | high |
| `0x000078f0` | `commport_isr_install` | Register ISR handle/callback + NVIC enable | high |
| `0x000096ce` | `commport_rx_complete_cb` | RX-complete SW-ring callback (task-notify) | high |
| `0x00002d34` | `commport_ring_drain` | TX/RX descriptor-ring drain (builds eDMA TCDs) | high |
| `0x00007624` | `commport_can_transmit` | TX/enqueue engine (rings + eDMA + FIFO kick) | medium |
| `0x000078f0` | `commport_isr_install` | Register per-instance ISR state (handle/cb/arg + NVIC) | high |
| `0x000085f8` | `commport_frame_enqueue` | Decode packed frame → {dlc,29-bit id,payload} → channel queue | high |
| `0x0000976e` | `commport_dma_ring_read_frame` | Read 8-byte frame out of the eDMA ring (2-channel descriptor) | high |
| `0x0000925a` | `prvBytesInBuffer` | FreeRTOS stream-buffer bytes-available — **vendor** (C removed) | high |
| `0x00009876` | `vmem_strncmp` | Hand-written bounded NUL-aware compare (strncmp-equiv) | high |
| `0x00001884` | `xfer_state_log_notify` | Transfer-complete: waiter reset + 0x3fd state toggle + log + wake | high |
| `0x00003cd0` | `vQueueAddToRegistry` | FreeRTOS queue-registry add (8 slots @0x20001dd8) — **vendor** | high |
| `0x00003338` | `vQueueDelete` | FreeRTOS queue delete (unregister + vPortFree) — **vendor** | high |
| `0x00006bfc` | `xTaskRemoveFromEventList` | FreeRTOS unblock from event list → ready list — **vendor** | high |
| `0x00008ca0` | `vListInsertEnd` | FreeRTOS list insert-end — **vendor** | high |
| `0x0000959e` | `uxListRemove` | FreeRTOS list-item remove — **vendor** | high |
| `0x000085cc` | `prvCopyDataFromQueue` | FreeRTOS dequeue copy (pcReadFrom advance + wrap) — **vendor** | high |
| `0x00007dc0` | `__aeabi_dmul` | libgcc double-precision multiply — **vendor** | high |
| `0x00007d10` | `__aeabi_f2d` | libgcc float→double convert — **vendor** (batch-23 d2f mislabel, corrected) | high |
| `0x000081e4` | `__aeabi_d2f` | libgcc double→float convert — **vendor** | high |
| `0x00008182` | `ddiv_special_operands` | libgcc ddiv NaN/Inf/denormal helper — **vendor** | high |
| `0x00008cb8` | `vListInsert` | FreeRTOS sorted list insert (by xItemValue) — **vendor** | high |
| `0x0000744c` | `vTaskNotifyGiveFromISR` | FreeRTOS notify-give + ready-list insert — **vendor** | medium |
| `0x00006854` | `xTaskIncrementTick` | FreeRTOS tick + delayed-list processing — **vendor** | high |
| `0x00000820` | `NVIC_SystemReset` | CMSIS AIRCR system reset (DSB + spin) — **vendor** | medium |
| `0x000086dc` | `xStreamBufferSpacesAvailable` | FreeRTOS stream-buffer free space — **vendor** | high |
| `0x000086fa` | `prvWriteBytesToBuffer` | FreeRTOS stream-buffer enqueue copy — **vendor** | high |
| `0x00008754` | `prvWriteMessageToBuffer` | FreeRTOS stream-buffer write (len prefix + payload) — **vendor** | high |
| `0x000084f6` | `commport_frame_encode_dispatch` | Encode 29-bit-id frame → driver vtable dispatch (TX) | high |
| `0x00008852` | `event_handler_dispatch` | Fire a registered handler (obj+0x1c → cb(arg)) under crit. | high |
| `0x00000884` | `int_pair_to_float` | VFP `(float)p[0] + (float)p[1]/1e6` (**decomp-c**, control.c — batch 27) | high |
| `0x00007f9c` | `dmul_special_operands` | libgcc dmul NaN/Inf/denormal helper — **vendor** | high |
| `0x0000336c` | `prvUnlockQueue` | FreeRTOS queue waiter-list wakeup — **vendor** | high |
| `0x00008538` | `device_send_chunked` | ≤8-byte chunked frame transmit (mod-8/16 seq counter) | high |
| `0x00008656` | `device_apply` | Build a record's command frame + transmit (single/chunked) | high |
| `0x00009178` | `device_store_words8808` | Write 8-byte field into device {0x08,0x88} + apply | high |
| `0x000090c0` | `device_cmd_read91` | Command write+verify @0x40 region, then read91 | high |
| `0x000095be` | `xQueueGenericCreate` | FreeRTOS queue/semaphore create (0x48 Queue_t) — **vendor** | high |
| `0x00008c8a` | `vListInitialise` | FreeRTOS list init (xListEnd, portMAX_DELAY) — **vendor** | high |
| `0x0000974e` | `xQueueCreateMutex` | FreeRTOS mutex create (create + give) — **vendor** | high |
| `0x0000879c` | `xSemaphoreCreateBinary` | FreeRTOS binary-sem create (tail-calls Create(1,0)) — **vendor** | high |

## Decompiled — batch 3 (`src/`, `include/`)

VanMoof GPIO + interrupt-dispatch glue and thin clock/peripheral accessors.
Both modules **faithful**, no vendor leakage, nothing deferred. All compile
clean. Direct MMIO reproduced verbatim (VanMoof bare-metal glue).

| Module | File | Functions | Note |
| --- | --- | --- | --- |
| gpio | `src/gpio.c`, `include/gpio.h` | `gpio_pin_config`, `gpio_irq_dispatch`, `gpio_bank_irq_trampoline_0..8`, `gpio_bank_irq_trampoline_9` | GPIO regs `0x4008E000`/PORT status. The 9 real trampolines share a macro. ⚠️ `gpio_bank_irq_trampoline_9` (`0x22b0`) is actually a **per-pin IRQ-config routine**, not a trampoline — rename to `gpio_pin_irq_config` in a follow-up. |
| clock | `src/clock.c`, `include/clock.h` | `GetSystemCoreClockSource`, `Adc_ReadCh_LPO1MHz`, `periph_clk_nvic_enable` | SCG `0x40000000`/`0x40013000`, clock-gate `0x40004000`, NVIC `0xE000E100`. `Adc_ReadCh_LPO1MHz` is a misnomer (1 MHz-LPO query, kept as OEM name). |

**Cross-module VanMoof glue left `extern` (future decomp targets, NOT vendor):**
`FUN_00008a64`/`FUN_00008aca` (PCC clock-gate enable), `FUN_00001ffc` (bank→index),
`FUN_000084cc` (PORT clock spin-wait), `FUN_000078d4` (NVIC ISER set). Runtime
tables/globals (`g_gpio_irq_table` @0x200014ac, `g_gpio_bank_*`, the periph-IRQ
table @0x1b34d and `g_port_pcc_index` @0x1b2e0 — both **off-image** >0x1a88b)
declared `extern`.

## Decompiled — batch 4 (`src/`, `include/`)

Cross-module VanMoof glue that batches 1–3 had left `extern`. Both modules
**faithful**, no vendor leakage. Renamed in Ghidra (program saved); the consuming
modules now `#include` these headers — the old per-file `extern` decls are gone
(single source of truth). All 11 modules compile clean.

| Module | File | Functions | Note |
| --- | --- | --- | --- |
| pcc | `src/pcc.c`, `include/pcc.h` | `pcc_gate_enable` (`0x8a64`), `pcc_gate_set` (`0x8aca`), `port_clock_wait` (`0x84cc`), `nvic_irq_enable` (`0x78d4`), `gpio_base_to_bank` (`0x1ffc`) | clock-gate / PORT handshake / NVIC ISER / GPIO bank map — pure verbatim MMIO. |
| util | `src/util.c`, `include/util.h` | `vmem_set` (`0x9866`) | the firmware's own byte-fill (`memset`-equivalent, 17 callers). Verified **not** libgcc — byte-granular, pointer-equality terminator, returns void. |

**Reconciled externs:** `gpio.c`, `clock.c`, `i2c.c` now `#include "pcc.h"`/`"util.h"`
instead of declaring local `extern`s. Remaining cross-module vendor symbol: `0x1bdc`
(FreeRTOS delay/yield) is still `extern` under three names (`rtos_delay_00001bdc`
in i2c.c, `FUN_00001bdc` in sensor.c, `busyWait_Ticks` in control.c) — vendor
(deferred); standardize when the RTOS boundary is finalized.

> Note: `ghidra/exports/user_ecu_program.json` is the mapping-phase snapshot (44
> fns) and predates batches 2–4; Ghidra itself is current (all batch fns renamed +
> saved). Refresh the JSON via a dump script when convenient. `progress.md` + the
> `src/` tree are the current source of truth.

## Decompiled — `main_SystemInit` (`0x44c0`, ~7.7 KB → `src/main.c`)

The C-runtime entry (called by `Reset_Handler`). Done via a **map → translate →
verify** pipeline; verdict **minor-issues** (the one concrete bug fixed). All 13
OEM sections, the 8 `xTaskCreate` sites, and the SysTick tail were verified
faithful. ~1620 lines; compiles clean. Every callee stays `extern`/`#include`d —
no callee body inlined.

**Sections (OEM order):** early GPIO pin-mux/PORT-PCR → `SystemClock_PllFlashInit`
+ SCG/PCC → peripheral clock-gates/NVIC + heap zero → allocator + control/IOM task
→ ADC clock-select + **LED-PWM (FTM)** config → I²C/IOM device-registration sweep →
**charger/PMIC** sampling + PRIMASK critical section → GPIO-IRQ regs + battery timers
→ **SAI/DMIC** clock-tree + L/R LED-ring & dmic tasks → DMIC decimator + audio task
→ **FlexCAN** comm port + dmic task → final descriptors + sensors/control tasks →
SHPR3 + **1 ms SysTick** (`RVR = SystemCoreClock/1000 - 1`) + `vPortStartFirstTask`
(no return). Peripheral details folded into `hardware.md`.

**Fixes applied this pass:**
- ✅ Restored a dropped MMIO write `*DAT_00005650 |= 0x10` (`0x547a`) the first
  translation left as a comment (verifier-caught).
- ✅ Reconstructed the **PRIMASK critical section** as real inline asm
  (`mrs/cpsid/msr`) — the decompiler's `isCurrentModePrivileged()`/
  `disableIRQinterrupts()` stubs don't exist in the firmware.
- ✅ Reconstructed the **VFP `vcvt`** idioms as C casts (`(float)(int32_t)…`,
  `(uint32_t)(… * 5.75f / 1.5f)`) — dropped the phantom `VectorSignedToFloat`/
  `VectorFloatToUnsigned` externs and the `in_fpscr` rounding-mode artifact.
- ✅ Volatile-cast the `0xaa`→`0x55` unlock double-write so `-Os` can't drop it.

**Open issues (refinement pass):**
- Four callees kept decompiler `func_0x….`/`FUN_*` names (`0x2754`, `0x6708`,
  `0x9876`, `0x1884`) — name when translated; confirm `0x6708` vs the header
  symbols near it.
- Task **entry/name pointers resolve into the off-image `0x1b2ff..0x1b341`
  region** (>`0x1a88b`); the enumerated entries (`l_led_ring 0x473c`, `sensors
  0xf98`, `dmic 0x4be8`) vs these high addresses need the upper ROM bank to
  reconcile. Params passed verbatim.
- Broader **MMIO-volatile audit** of the `DAT_*` scratch pointers in `main.c` is
  pending (MMIO via the `MMIO32()` macro is already `volatile`; the dead-store-
  prone double-write is fixed).
- `SystemClock_PllFlashInit` (`0x1244`) is **done** (batch 6). The registration
  helper `FUN_00008594` → **`registry_add`** is **done** (batch 7); `FUN_00006a10`
  → `pvPortMalloc` is **vendor** (deferred). `main.c` keeps both as decompiler-form
  local externs (ABI-compatible) — standardize at link prep.

## Decompiled — batch 6 (`src/`, `include/`)

Two `main_SystemInit` dependencies. Both compile clean; `main.c` reconciled.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| sysclock | `src/sysclock.c`, `include/sysclock.h` | `SystemClock_PllFlashInit` (`0x1244`) | **faithful** (verifier corrected a mis-grouped table — runtime-irrelevant). SCG/SPLL→96 MHz, FTFC flash + factory IFR trim, publishes `SystemCoreClock`. Calls `clock_div_program` (`0x110c`) + `busy_wait` (`0x84b2`) — both **done in batch 7**; now `#include`d, externs dropped. |
| gpio_irq | `src/gpio_irq.c`, `include/gpio_irq.h` | `gpio_irq_register` (`0x159c`) | **faithful**. Fills a **separate** RAM registration table @`0x20000668` (NOT the dispatch table) — `+0 seq, +1 bank, +2 pin, +3 idx, +4 pvPortMalloc(0xc)`. |

**Reclassified vendor:** `FUN_00006a10` → **`pvPortMalloc`** — confirmed FreeRTOS
heap_4 (calls `vTaskSuspendAll`/`xTaskResumeAll`). Renamed in Ghidra; now counted
`deferred (vendor)`. (`main.c` still declares its many call sites as `FUN_00006a10`
— a vendor extern, fine for `-c`; standardize at link prep.)

**Reconciled:** `0x159c` renamed in Ghidra; `main.c` `FUN_0000159c` calls →
`gpio_irq_register`. `SystemClock_PllFlashInit` name already matched `main.c`.

> The `sysclock` flash-divider table @`0xa4a0` lies in the same odd-looking rodata
> band as the `q16_exp`/`irqn` tables (`0xa4a0..0xa540`, repeating `0x48004800`),
> but `sysclock` uses it **benignly** — entry[0]'s threshold matches first, giving
> flash divider `1` regardless. So that band is reproduced verbatim and is only a
> concern for `q16_exp`/`q16_sigmoid`/`irqn_to_gpio_index`, whose algorithms expect
> structured content the bytes don't provide (still flagged table-unverified).

## Decompiled — batch 7 (`src/`, `include/`)

The remaining `sysclock`/`main_SystemInit` leaf helpers. Five functions, all
VanMoof glue, faithful (translated directly from the disassembly + decompiler
with all literals resolved). All compile clean; both consumers reconciled.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| clock | `src/clock.c`, `include/clock.h` | `clock_div_program` (`0x110c`) | **faithful**. Unpacks up to two 12-bit `{selector[7:0], value[11:8]}` divider descriptors; programmed divider is `value-1`. Selector `0x3f`→bit0 of `0x40020098`, `0x3e`→bits5:4 of `0x4002009c`, else byte to divider-array word `0x40000260 + sel*4`. `DAT_0000116c` resolved = `0x40020000`. |
| util | `src/util.c`, `include/util.h` | `vmem_copy` (`0x984c`) | **faithful**. Hand-written forward byte-copy (`memcpy`-equivalent), sibling of `vmem_set` (`0x9866`); end-pointer terminator, no overlap/word fast path. NOT libgcc. |
| util | (same) | `busy_wait` (`0x84b2`) | **faithful**. `do { n--; } while (n);` spin delay; `n==0` underflows to 2^32 (verbatim). |
| registry | `src/registry.c`, `include/registry.h` | `registry_find` (`0x82c6`) | **faithful**. Linear scan over `count` 0x2c-byte slots calling `reg->cmp(key, slot)`; returns index or `0xffffffff`. Header struct `{count, capacity, cmp, slots}` (offsets 0/4/8/0xc). |
| registry | (same) | `registry_add` (`0x8594`) | **faithful**. Dedup via `registry_find` on `entry+0xc` (key), then `vmem_copy` the 0x2c-byte entry into `slots[count++]`. Returns 0 / `0xffffffff` (NULL, full, or dup). Called ~30× from `main_SystemInit`. |

**Reconciled:** `sysclock.c` now `#include`s `clock.h` + `util.h` (dropped the
`FUN_0000110c`/`FUN_000084b2` externs, renamed call sites). `main.c`
name-renamed `FUN_00008594`→`registry_add`, `FUN_000084b2`→`busy_wait`,
`FUN_0000110c`→`clock_div_program` (its `clock_div_program` extern dropped — it
already `#include`s `clock.h`; `registry_add`/`busy_wait` stay as decompiler-form
local externs, ABI-compatible). All **15 modules** compile clean.

> The `0x2c`-byte entry record and the `+0xc` key offset are shared by
> `registry_find`/`registry_add` and the comparator (`reg->cmp`); the comparator
> bodies and the registry initialisers in `main_SystemInit` are the natural
> follow-up (started in batch 8).

## Decompiled — batch 8 (`src/`, `include/`)

Completes the registry **read** API. Two faithful VanMoof functions added to
`registry.c`; one neighbour reclassified vendor. All 15 modules compile clean;
`main.c` reconciled (name-only).

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| registry | `src/registry.c`, `include/registry.h` | `registry_lookup` (`0x82f2`) | **faithful**. `registry_find` → slot pointer (`slots + idx*0x2c`) or NULL. Decompiler models it 1-arg because the key (r1) is forwarded untouched; true ABI is `(reg, key)`. |
| registry | (same) | `registry_lookup_value` (`0x830a`) | **faithful**. Stacks an inline 2-word key `{key0,key1}` and forwards to `registry_lookup`; NULL when `reg==NULL`. The widely-used "get device by id" accessor (called from `controlTask`, `0x8e76`, `0x9178`, `0x8fd6`, `0x8e0a`, `main`). |

**Reclassified vendor:** `FUN_00008d54` → **`xQueueSemaphoreTake`** — FreeRTOS
semaphore-take (privilege/IRQ check, BASEPRI-guarded `uxMessagesWaiting` decrement
at `+0x38`, waiting-task list `+0x10`, PendSV `0xE000ED04 = 0x10000000`; thread
path calls `rtos_sem_take`). Renamed in Ghidra, counted `deferred (vendor)`.
`main.c` keeps it as a decompiler-form local extern.

**Reconciled:** `0x82f2`/`0x830a`/`0x8d54` renamed in Ghidra (saved); `main.c`
name-renamed to `registry_lookup`/`registry_lookup_value`/`xQueueSemaphoreTake`.
⚠️ `main.c`'s local `registry_lookup_value` extern is **2-arg** (decompiler
dropped the 3rd register arg `key1`); the real ABI is 3-arg `(reg, key0, key1)`.
Harmless for `-c`; fix at link prep with the other decompiler-form prototypes.

> **⚠️ Registry comparator — anomaly (open, NOT a mis-carve).** Investigated via
> a 4-angle read-only workflow + manual cross-check; all high-confidence and
> agreeing. Findings:
> - There is **exactly one** device registry, RAM `0x20001734` (capacity `0x20`).
>   `DAT_00004f00`/`52e0`/`5968`/`5ffc`/`6284` are all the *same* base — repeated
>   constant-pool copies, **not** five registries.
> - Its `cmp` field (`reg+8` = `0x2000173c`) is written **once**, at
>   `main_SystemInit:0x476a`, from the literal at `0x485c` = **`0x8eb5` → thumb
>   `0x8eb4`**. `search_byte_patterns "b5 8e 00 00"` finds that pointer in exactly
>   one place in the whole image.
> - **`FUN_00008e76` is correctly carved and is NOT the comparator.** It has a
>   clean prologue (`push {r4-r9,lr}; sub sp,#0x1c`), the prior function ends
>   cleanly at `0x8e72`, and it has real direct callers (`0x8fc2`, `0x529c`). It
>   is a registry **consumer** — a device read/probe routine that itself calls
>   `registry_lookup_value`, takes a semaphore and copies a record.
> - `0x8eb4` is **genuine interior code** of that function (`add r0,sp,#8; bl
>   vmem_set` — the `if (1 < local_30)` clear-buffer path; `bls 0x8ebe` at
>   `0x8eae` falls through it). It is **not comparator-shaped** (clobbers r0/key,
>   never reads r1/slot or `slot+0xc`) and would corrupt the stack if dispatched.
>
> So this is **not** a Ghidra over-extension and there is **no comparator to
> re-carve** — `0x8eb4` is provably interior. The `cmp` literal is either an
> aliased/latent value or the registry's `registry_find` is never actually
> dispatched at runtime (if it were, `cmp(0x8eb4)` would crash). Resolving *why
> it doesn't crash* needs **dynamic/emulation analysis**, not static re-carving.
> **Action: leave flagged; do NOT mutate `FUN_00008e76`.** The device-read family
> `FUN_00008e76` (+ `0x8e0a`/`0x8f76`/`0x8fd6`) is now translated (batch 9); the
> `cmp=0x8eb4` runtime puzzle remains the only open thread (dynamic analysis).

## Decompiled — batch 9 (`src/`, `include/`)

The registry **consumers** the batch-8 anomaly investigation surfaced: a
registry-backed device-record cache (read/write a part over a transient I²C bus
session, refresh the cached copy under a per-device semaphore). New module
`device.{c,h}`; plus the third hand-written libc-trio sibling `vmem_cmp`. All
faithful (translated from the disassembly with every literal resolved); all 16
modules compile clean; `main.c` reconciled (name-only). One neighbour
reclassified vendor.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| util | `src/util.c`, `include/util.h` | `vmem_cmp` (`0x982c`) | **faithful**. Forward byte `memcmp`: end-pointer terminator on the first operand, returns 0 or the signed first-mismatch difference. Completes the `0x982c`/`0x984c`/`0x9866` (cmp/copy/set) trio. NOT libgcc. |
| device | `src/device.c`, `include/device.h` | `device_read_record87` (`0x8e76`) | **faithful**. Open session → read 14 B @ sub-addr `0x30` → reject leading byte > 1 → cache payload `buf[1..13]` into the device record (`slot[0]`) under `slot[1]` semaphore → optional `vmem_cmp` vs `expect`. Lookup tag `{id,0x87,0}`. |
| device | (same) | `device_read_record91` (`0x8fd6`) | **faithful**. Same shape, 16 B @ sub-addr `0x40`, caches the full record (no length-byte gate), tag `{id,0x91,0}`. |
| device | (same) | `device_store_field8c0` (`0x8e0a`) | **faithful**. Lookup `{0xc0,0x08,0}` → take sem → store 3-byte field (`strh`+`strb`) → apply/notify hook `FUN_00008656` → give sem (OEM tail-call). |
| device | (same) | `device_cmd_read87` (`0x8f76`) | **faithful**. Build 14-B frame `0x01‖payload[13]` → write+verify (`FUN_00008b12`) → commit (`FUN_00002938`) → close → `device_read_record87` w/ frame as `expect`. |

**Reclassified vendor:** `FUN_000097f4` → **`rtos_sem_give_dispatch`** — FreeRTOS
semaphore **give** (privilege/exception-number check; thread path → `rtos_sem_give`
(`0x6ec0`), ISR path → internal + PendSV `0xE000ED04 = 0x10000000`). The give
counterpart of `xQueueSemaphoreTake`. Renamed in Ghidra, counted `deferred (vendor)`.

**Left `extern` (VanMoof I²C bus-session glue — future decomp targets, NOT
vendor):** `FUN_000028c8` (session open, `pvPortMalloc(0x3c)` + vtable init),
`FUN_00002910` (bus transfer `(sess,buf,addr,len)` — vtable dispatch),
`FUN_00002938` (session commit/flush), `FUN_000087f2` (free-if-non-NULL close),
`FUN_00008656` (device apply/notify hook, uses `mgr+0x594`/`+0x5a4`),
`FUN_00008b12` (write-then-read-back-verify, uses `vmem_cmp`). Declared in
`device.c` in decompiler form (ABI as used at the call sites).

**Reconciled:** `0x8e76`/`0x8fd6`/`0x8e0a`/`0x8f76`/`0x982c`/`0x97f4` renamed in
Ghidra (saved). `main.c` name-renamed `FUN_00008e76`→`device_read_record87`,
`FUN_00008fd6`→`device_read_record91`, `FUN_000097f4`→`rtos_sem_give_dispatch`
(kept as decompiler-form local externs, ABI-compatible — standardize at link prep).

> The device record slot's first two words are `{data_ptr, semaphore}` and its
> registry key is at `+0xc` (per `registry.*`). The `dev_handle` passed to the
> readers is `{registry*, id}` (offsets +0/+4). The registry these consume is the
> single `0x20001734` device table from the batch-8 note — i.e. these are the
> live callers that would dispatch its (anomalous) `cmp`, yet the call sites only
> populate the low 3 bytes of the lookup key.

## Decompiled — batch 10 (`src/`, `include/`)

The I²C **bus-session layer** beneath the batch-9 device accessors: a transient
heap-allocated session driven through a single global driver manager
(`g_bus_mgr`, fixed at `0x1301fe00`). Every transfer primitive dispatches through
the manager's vtable (`mgr+0x10`), selecting one of two method variants (A/B) by
a board/bus selector byte at `mgr+0x06`. New module `bus.{c,h}`; plus the generic
free wrapper `mem_free` in `util.c`. All faithful; all **17 modules** compile
clean; `device.c` + `main.c` reconciled. One neighbour reclassified vendor.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| bus | `src/bus.c`, `include/bus.h` | `bus_variant_b` (`0x288c`) | **faithful**. `g_bus_mgr->variant == 3` → pick the "B" vtable methods (each `0xc` above its "A" sibling). |
| bus | (same) | `bus_transfer` (`0x2910`) | **faithful**. Tail-dispatch read/xfer `(sess,buf,addr,len)` to driver vtable `+0x2c`/`+0x38`. |
| bus | (same) | `bus_write_commit` (`0x6538`) | **faithful**. Tail-dispatch write `(sess,desc,0)` to vtable `+0x24`/`+0x30`. |
| bus | (same) | `bus_probe_read` (`0x65dc`) | **faithful**. Tail-dispatch probe `(sess,buf,0,0x200)` to vtable `+0x40`/`+0x4c`. |
| bus | (same) | `bus_session_init` (`0x289c`) | **faithful**. `state(+0x28)=0x60`, run driver init (`vtable+4`), then if `timing(+4)==0xa0000` recompute `0xa0000 − 17·param(+0xc)`. Returns init status (decompiler dropped the indirect-call return). |
| bus | (same) | `bus_mode_autodetect` (`0x89c0`) | **faithful**. Probe modes 1 then 2 (`+0x24`), read metric from page word 1, latch `{metric,mode}` at `+0x20/+0x24` for the larger. Returns the last probe status (the decompiler's `void` is wrong — `0x28c8` gates on it). |
| bus | (same) | `bus_session_open` (`0x28c8`) | **faithful**. `pvPortMalloc(0x3c)` → init → driver open (`vtable+0x1c`/`+0x28`) → autodetect; frees + returns NULL on any failure. |
| bus | (same) | `bus_session_commit` (`0x2938`) | **faithful**. RMW of the 0x200 config page: `set` → OR `0x70` into word 0 + sentinel `{0xff2000df,0xffff0000}` at words 4/5; `!set` → clear words 4/5. Writes back only if changed. |
| util | `src/util.c`, `include/util.h` | `mem_free` (`0x87f2`) | **faithful**. `if (p) vPortFree(p)` — the firmware's generic NULL-guarded free. |

**Reclassified vendor:** `FUN_00006b9c` → **`vPortFree`** — FreeRTOS heap_4 free
(block-link `heapBLOCK_ALLOCATED` check, free-list reinsert, `xFreeBytesRemaining`
accounting). Renamed in Ghidra, counted `deferred (vendor)`. (`pvPortMalloc`
`0x6a10` was already deferred — batch 6.)

**Driver-vtable map (`*(g_bus_mgr+0x10)`), variants 0xc apart:** init `+0x04`;
open `+0x1c`/`+0x28`; write `+0x24`/`+0x30`; xfer `+0x2c`/`+0x38`; probe
`+0x40`/`+0x4c`. The concrete method targets are runtime-populated by the
`main_SystemInit` device-registration sweep and are not statically resolvable;
modeled as a typed function-pointer table in `bus.h` (no `void*`→fn-ptr casts).

**Reconciled:** `device.c` now `#include`s `bus.h`/`util.h` and uses
`bus_session_open`/`bus_transfer`/`bus_session_commit`/`mem_free` over a typed
`bus_session_t *` (the four `FUN_*` externs dropped); `FUN_00008656`/`FUN_00008b12`
(device write/apply) stay `extern` (batch 11). `main.c` name-renamed
`FUN_000087f2`→`mem_free`, `FUN_0000289c`→`bus_session_init`,
`FUN_00006b9c`→`vPortFree` (decompiler-form local externs kept, ABI-compatible —
standardize at link prep; note `vPortFree`'s pointer arg is dropped by the
decompiler at `0x4760`).

> All five constant-pool literals that reach the manager (`0x290c`/`0x28c4`/
> `0x6560`/`0x2934`/`0x2898`/`0x660c`) hold the **same** base `0x1301fe00` — one
> global manager, mirroring the single-registry aliasing seen in batch 8. The
> manager/driver/session structs are runtime data (populated at init); only the
> dispatched offsets are modeled.

## Decompiled — batch 11 (`src/`, `include/`)

The **device write/apply** helpers that `device.c` had left `extern` — closing
the I²C device/bus cluster (registry → device records → bus session → write).
Three faithful functions; `device.c` now carries **no `FUN_*` externs at all**.
All **17 modules** compile clean.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| bus | `src/bus.c`, `include/bus.h` | `bus_page_write_verify` (`0x8b12`) | **faithful**. Read the 0x200 page → splice `len` bytes of `buf` at page `+0x30+off` → `bus_write_commit` → re-read at sub-addr `(off+0x30)&0xff` → `vmem_cmp` verify. Returns 0 only when verified. A bus-session op (no registry), so it lives in `bus.c`. |
| device | `src/device.c`, `include/device.h` | `device_apply` (`0x8656`) | **faithful**. Builds a command frame (key from `rec+0xc`, payload per `rec->type`) and transmits via the manager's channel (`mgr+0x594`, send ptr `mgr+0x5a4`). type 0 → single-shot (+ optional `rec->aux`); type 1/2 → `device_send_chunked`; else → -1; NULL arg → -2. |
| device | (same) | `device_send_chunked` (`0x8538`) | **faithful**. Splits `rec->data[0..length)` into ≤8-byte frames, dispatching `channel->send(channel, frame)` per chunk; the frame sequence nibble wraps mod 16 (`rec->type==2`) or mod 8. `first==0` sends a single empty frame. |

**Record/channel layout pinned down (from `0x8656`/`0x8538` disasm):** the
0x2c-byte registry slot is now fully typed in `device.h` (`dev_record_t`):
`+0x00` data, `+0x04` sem, `+0x08` length, `+0x0c` key `{id,type,0}`, `+0x10`
type (0/1/2), `+0x20` aux ptr, `+0x24` aux len. The device manager embeds the
registry at offset 0 and a transmit channel at `+0x594` whose `send` method ptr
is at `+0x5a4` (= channel`+0x10`); the chunked and single-shot paths invoke the
**same** method.

**Reconciled:** `device.c` `device_store_field8c0` now calls `device_apply`,
`device_cmd_read87` now calls `bus_page_write_verify`; the last two `FUN_*`
externs are gone. `device_apply` is exposed in `device.h` (it has non-source
callers `0x9178` and the control task `0x370`, to be translated later);
`device_send_chunked` stays module-internal but un-inlined (separate OEM symbol).
No `main.c` change (none of the three are referenced from translated sources).

> The I²C **device/bus** stack is now fully reconstructed end-to-end: `registry.*`
> (find/add/lookup) → `device.*` (record read/write/apply) → `bus.*` (session
> open/transfer/commit/verify, variant-dispatched through the `0x1301fe00`
> manager). The only remaining glue in this cluster is the concrete driver-vtable
> targets, which are runtime-populated (not statically resolvable).

## Decompiled — batches 12–13 (`src/`, `include/`)

Two more `device.c` accessor siblings rounding out the family.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| device | `src/device.c`, `include/device.h` | `device_store_words8808` (`0x9178`) | **faithful**. Look up device `{0x08,0x88}` → take sem → store the caller's two words (8 bytes) into the cached record → `device_apply` → release sem; **returns** `device_apply`'s status (or -1). The 8-byte sibling of `device_store_field8c0`. Only caller: `main_SystemInit`. |
| device | (same) | `device_cmd_read91` (`0x90c0`) | **faithful**. The 16-byte twin of `device_cmd_read87`: `bus_page_write_verify` the payload into the device's `0x40` region (`off=0x10`) → `bus_session_commit` → `device_read_record91` w/ payload as `expect`. Returns the read result or -1. No callers in the current image. |

**Record-region symmetry confirmed:** the `off` arg of `bus_page_write_verify`
selects the page region — `device_cmd_read87` writes `0x30` (`off=0`, paired with
`device_read_record87`'s sub-addr `0x30`); `device_cmd_read91` writes `0x40`
(`off=0x10`, paired with `device_read_record91`'s sub-addr `0x40`).

**Reconciled:** `main.c` name-renamed `FUN_00009178`→`device_store_words8808`
(2-arg decompiler-form local extern kept — the 3rd key word is the OEM's
don't-care r2; standardize at link prep). `device_cmd_read91` has no source
callers, so no `main.c` change.

## Batch 25 — FreeRTOS classification sweep (no new C)

With the VanMoof app layer essentially reconstructed, the remaining `FUN_*` were
swept and classified. No new VanMoof code was written; this batch is Ghidra
hygiene + accurate `deferred (vendor)` accounting.

**GPIO bank trampolines — reconciled (VanMoof, C already in `gpio.c`):** the
`0x2100..0x2280` step-`0x30` trampoline block (per `gpio.c`'s own range comment)
had 7 still-`FUN_` siblings; renamed to match the existing macro C:
`0x2130→_1`, `0x2160→_2`, `0x21c0→_4`, `0x21f0→_5`, `0x2220→_6`, `0x2250→_7`,
`0x2280→_8` (slot `_3` = `0x2190` is `commport_irq_dispatch_inst3`, the CAN
instance; `_0`=`0x2100`, `_9`=`0x22b0` pin-config already named).

**Classified vendor + renamed (FreeRTOS kernel / libgcc / CMSIS):**

| Address | Name | Evidence |
| --- | --- | --- |
| `0x62e8` | `prvInsertBlockIntoFreeList` | heap_4 free-list coalesce/merge walk. |
| `0x637c` | `prvResetNextTaskUnblockTime` | publishes next-unblock time from delayed-list head. |
| `0x63a0` | `xTaskGetSchedulerState` | returns 0/1/2 from the suspend + running flags. |
| `0x6498` | `xTaskCheckForTimeOut` | xTimeOut compare against the tick + overflow counts. |
| `0x6b44` | `prvCheckForValidListAndQueue` | timer module lazy-init: 2× `vListInitialise` + `xQueueGenericCreate(10,0x10)` + registry. |
| `0x6cdc` | `prvIdleTask` | infinite loop freeing terminated TCBs (`vPortFree` stack + TCB) then yield. |
| `0x6d34` | `prvAddCurrentTaskToDelayedList` | `uxListRemove` + `vListInsert` into delayed/overflow list by wake time. |
| `0x6da4` | `vTaskPlaceOnEventList` | `vListInsert` onto event list + delay. |
| `0x89f8` | `xEventGroupSetBits` | OR bits, walk waiters with AND/OR conditions, unblock via `0x6c70`. |
| `0x6c70` | `xTaskRemoveFromUnorderedEventList` | event-group unblock (value | 0x80000000); called by `xEventGroupSetBits` + `commport_teardown`. |
| `0x636c` | `vTaskSuspendAll` | `uxSchedulerSuspended++`-style. |
| `0x694c` | `xTaskResumeAll` | resume + pended-tick catch-up. |
| `0x1bdc` | `vTaskDelay` | (was the `rtos_delay` extern). |
| `0x952e` | `vPortYield` | PendSV set (`0xE000ED04=0x10000000`) + DSB/ISB. |
| `0x9652` | `xQueueGiveFromISR` | message-count++ + `+0x45` lock sentinel + waiter wake. |

**Residual `FUN_*` (8 — left for a future pass):**

| Address | Disposition |
| --- | --- |
| `0x6dc4` | FreeRTOS task-notify **wait** (vendor) — no source refs; rename pending exact symbol. |
| `0x74e0` | FreeRTOS task-notify **give** (vendor) — sibling of `0x744c`; rename pending. |
| `0x6e20` | FreeRTOS queue receive/copy internal (vendor) — still `extern FUN_00006e20` in `comm_txisr.c`. |
| `0x642c` | configASSERT/trap stub (`vPortRaiseBASEPRI` + spin) — vendor-ish. |
| `0x9544` | bad-data/configASSERT trap stub — vendor-ish. |
| `0x15e0` | **FreeRTOS** software-timer create (`pvPortMalloc(0x28)`, posts via `0x6fd0`) — confirmed vendor by the orphaned-gap sweep (siblings at `0x16c0-0x1883` are timers.c). Rename pending exact symbol. |
| `0x6fd0` | **FreeRTOS** timer command dispatch / list-swap (uses `vPortRaiseBASEPRI`/`xTaskGetSchedulerState`; the `prvTimerTask`+`prvProcessExpiredTimer` daemon is at `0x7012-0x7287`) — confirmed vendor by the sweep. The cmd-6 → `commport_rx_complete_cb` path is an app callback fired *through* the stock timer service, not evidence of VanMoof authorship. Rename pending. |
| ~~`0x67f4`~~ | **DONE** → `nvic_clockgate_bringup` in `pcc.c`. The `+0x84`/`+0x184` are NVIC ICER1/ICPR1 (disable + clear-pending IRQ 33); enables clock-gate bit 5 of the 0x40004000 block with DSB/ISB. No args (decompiler params were phantom). |

**Reconciled:** `comm.c` externs/call-sites `FUN_0000636c`→`vTaskSuspendAll`,
`FUN_0000694c`→`xTaskResumeAll`, `FUN_00006c70`→`xTaskRemoveFromUnorderedEventList`;
`store.c` `FUN_000087fa`→`xfer_waiter_reset` (a queue-reset+signal wrapper — its
core is `xQueueGenericReset`-shaped but the wrapper is VanMoof-vs-vendor
ambiguous, left extern). All 20 modules compile clean; Ghidra renamed (24) + saved.

> **State of the image:** the VanMoof application layer (I²C/device/bus, FOTA/
> storage, comm-port/CAN TX-RX + frame codecs, registry, control, sensor, crc,
> q16, gpio, clock, sysclock) is reconstructed. The remaining frontier is the
> **fused task bodies** (LED-ring `0x473c`/`0x4768`, sensors `0xf98`, dmic
> `0x4be8`) and the **PWM/SAI-DMIC driver bodies** inside `main_SystemInit` —
> these need careful carving (worklist 1 + 6) — plus the deferred `int_pair_to_float`
> (`0x884`) and the 3 ambiguous residuals above.

## Decompiled — batch 24 (`src/`, `include/`) — queue-write/frame set + FreeRTOS stream-buffer correction

A batch-24 analyze→verify pass on the "comm-port queue-write" set delivered a
**clean-room correction**: the adversarial verifiers proved (and a manual diff
against canonical FreeRTOS confirmed) that the "comm-port payload queue" is
**verbatim FreeRTOS `stream_buffer.c` (v10.0–10.2.x)** — the struct is an exact
`StreamBuffer_t` (`xTail@0, xHead@4, xLength@8, xTriggerLevelBytes@0xc,
xTaskWaitingToReceive@0x10, xTaskWaitingToSend@0x14, pucBuffer@0x18, ucFlags@0x1c`),
and the bit0="4-byte length prefix" is `sbFLAGS_IS_MESSAGE_BUFFER`. The
version-specific 3-arg `prvWriteBytesToBuffer` (reads/writes `xHead` internally,
returns count) dates it pre-v10.3.

**Removed (reclassified vendor, C deleted from `comm.c`/`comm.h`):** three
functions earlier reconstructed as VanMoof were FreeRTOS kernel code —
`commport_queue_receive` (`0x7550` = `xStreamBufferReceive`, batch 21),
`queue_ring_copyout` (`0x91ec` = `prvReadBytesFromBuffer`, batch 21),
`commq_bytes_used` (`0x925a` = `prvBytesInBuffer`, batch 23). The orphaned
`commq_t` model, `COMMQ_CUR_TCB` macro and the FreeRTOS notify externs were
removed too. They link against upstream FreeRTOS. (Approved by the user.)

**Translated (genuine VanMoof, → `comm_txisr.c`):**

| Module | Function | Verdict / note |
| --- | --- | --- |
| comm_txisr | `commport_frame_encode_dispatch` (`0x84f6`) | **faithful** (verify moved a stray `(void)a`). TX counterpart to `commport_frame_enqueue`: reads the 32-bit id at `frame+4`, re-splits into 4 big-endian ID bytes (only low byte masked to 5 bits) into a 16-byte record `{id[0..3], hdr@4, driver@8, d@12}`, copies `frame[0]` payload bytes from `frame+8`, then `(*driver)(driver,&record)` via a typed fn-ptr; returns `(result==0)`. |
| comm_txisr | `event_handler_dispatch` (`0x8852`) | **faithful**. Under a critical section reads the control block at `obj+0x1c`; outside it tail-calls `block->handler(block->arg)` (block+4 / block+8) when non-NULL. Generic registered-callback dispatch (configASSERT on NULL obj); modeled with a typed handler fn-ptr. |

**Deferred (named, no C yet):** `int_pair_to_float` (`0x884`) — a VFP helper
returning `(float)p[0] + (float)p[1]/1e6`; verified faithful but its physical
quantity / module home is unclear (caller is the control/comms task). Named in
Ghidra; translate once a home is determined (poor fit for the Q16 `control.c`).

**Reclassified vendor — batch 24 (no C):**

| Address | Name | Evidence |
| --- | --- | --- |
| `0x86dc` | `xStreamBufferSpacesAvailable` | `xLength + xTail − 1 − xHead` wrap — FreeRTOS free-space. |
| `0x86fa` | `prvWriteBytesToBuffer` | 3-arg pre-v10.3 form (reads/writes `xHead`, returns count). |
| `0x8754` | `prvWriteMessageToBuffer` | message-buffer write: 4-byte length prefix (when `ucFlags`&1) + payload; callers are `xStreamBufferSend`. |
| `0x926c` | `xStreamBufferSend` | calls `xStreamBufferSpacesAvailable` + `prvWriteMessageToBuffer` (corrected from the batch-19 `xQueueGenericSend` label; `store.c` reconciled). |
| `0x8182` | `ddiv_special_operands` | libgcc double NaN/Inf/denormal helper, called by `aeabi_ddiv_softfloat`. |
| `0x81e4` | `__aeabi_d2f` | double→float (32-bit result, `0x7f800000`/`0x7fc00000`). |
| `0x7d10` | `__aeabi_f2d` | **batch-23 correction** — float→double (`^0x380…` = +896 exponent rebias); was mislabeled `__aeabi_d2f`. |
| `0x8cb8` | `vListInsert` | FreeRTOS sorted list insert by `xItemValue` (0xffffffff end marker). |
| `0x744c` | `vTaskNotifyGiveFromISR` | notify-state 1→2 + ready-list insert + `xHigherPriorityTaskWoken` out; called by `xStreamBufferSend`. |
| `0x6854` | `xTaskIncrementTick` | tick++ + overflow/delayed-list processing; called by `xTaskResumeAll`. |
| `0x820` | `NVIC_SystemReset` | CMSIS AIRCR write (preserve PRIGROUP) + DSB + spin. |

**Reconciled:** `comm.c` lost the 3 functions + `commq_t`/macro/notify-externs
(header note added); `comm.h` lost `commq_t` + the 3 prototypes; `comm_txisr.c`
gained the 2 functions + `vPortEnterCritical`/`vPortExitCritical` externs;
`store.c` renamed its `FUN_0000926c` extern → `xStreamBufferSend`. Ghidra renamed
+ saved (17 functions); plate comment on `0x7550` records the cluster + the
reconstruction-then-removal. All 20 modules compile clean.

> **Lesson:** the `StreamBuffer_t`-shaped object (read/write/size + 2 task
> handles + storage + flags-bit0-len-prefix) is the FreeRTOS stream/message-buffer
> fingerprint — not a VanMoof byte-ring protocol. The VanMoof producers/consumers
> (`event_report`, the comms tasks) are real; the buffer machinery is vendor.

## Decompiled — batch 23 (`src/`, `include/`) — comm-port frame/ring + util tail

Five VanMoof functions surfaced while closing out the comm-port `extern` list,
translated via an analyze→adversarial-verify workflow (7 candidates × 2 agents).
The verify pass **confirmed a hard vendor boundary**: two 8-slot-table functions
that share the array `0x20001dd8` are the FreeRTOS **queue registry** — so both
are vendor, not the "custom table" one first looked like (see below). All 20
modules compile clean.

| Module | Function | Verdict / note |
| --- | --- | --- |
| comm_txisr | `commport_frame_enqueue` (`0x85f8`) | **faithful**. Decodes a packed frame `[b0 b1 b2 b3 dlc payload…]` into a 16-byte message `{dlc@0, 29-bit ext-id@4, payload@8}` (id = `b0<<21|b1<<13|b2<<5|(b3&0x1f)`, DLC clamped to 8) and posts it to the channel queue at `*(chan+0x14)` via `rtos_sem_give` (send-to-back). Reached via fn-ptr. |
| comm_txisr | `commport_dma_ring_read_frame` (`0x976e`) | **faithful** (verify-corrected a phantom 5-arg `vmem_copy` → the real 3-arg; dropped a phantom 4th param). Reads 8 bytes out of the eDMA ring at a computed offset (per-channel descriptor `+0xa0…/+0xb0…`, stride from the `+0xbc` mode code, base `*(obj+0x200)`), stores the tail ptr at `out+8`, updates the head index. Reached via fn-ptr. |
| comm | ~~`commq_bytes_used` (`0x925a`)~~ | **[BATCH-24 CORRECTION: this is FreeRTOS `prvBytesInBuffer` — reclassified vendor, C removed from `comm.c`.]** Originally read as a wrap-aware byte-ring fill level `(write+size−read)`. |
| util | `vmem_strncmp` (`0x9876`) | **faithful**. Fourth hand-written string/mem sibling (after cmp/copy/set): bounded NUL-aware forward compare with the house `s2-1` pre-increment idiom — strncmp semantics. Used by `main_SystemInit` (6-byte compare). |
| store | `xfer_state_log_notify` (`0x1884`) | **faithful**. Transfer-completion handler: resets the flagged waiter queues, toggles the connection/state flag on the `0x3fd` "up" code (appending the transition to the event log via `log_append_event`), then hands the completed record to blocked tasks. Grouped in `store.c` for the `log_append_event` coupling; reached via fn-ptr from `main_SystemInit`. Resolved globals: `g_xfer_waiter_a` `0x200006b4`, `g_xfer_waiter_b` `0x20000970`, `g_xfer_state_flag` `0x200070e9`. |

**Reclassified vendor (10, no C — named in Ghidra, counted `deferred`):** the verify
pass + a manual cross-check resolved a cluster of FreeRTOS/libgcc neighbours:

| Address | Name | Evidence |
| --- | --- | --- |
| `0x3cd0` | `vQueueAddToRegistry` | 8-slot `{pcQueueName,xHandle}` registry @`0x20001dd8`; caller pairs `xQueueGenericCreate` result + a flash name-string ptr. Workflow high-conf. |
| `0x3338` | `vQueueDelete` | **Override** of the workflow's "vanmoof" call: scans the *same* registry `0x20001dd8` by handle, clears the slot, then `vPortFree`s the handle = unregister-inlined + heap free. Called by `commport_teardown`; matches the old `comm.c` comment "queue delete + free". |
| `0x6bfc` | `xTaskRemoveFromEventList` | uxListRemove + `vListInsertEnd(pxReadyTasksLists[prio·0x14])` + `uxTopReadyPriority`/`xYieldPending` bookkeeping. |
| `0x8ca0` | `vListInsertEnd` | list insert-before-index (confirmed via `0x6bfc`). |
| `0x959e` | `uxListRemove` | list-item unlink (confirmed via `0x6bfc`). |
| `0x85cc` | `prvCopyDataFromQueue` | called only by `xQueueSemaphoreTake`; pcReadFrom += uxItemSize, wrap to pcHead, `vmem_copy`. |
| `0x7dc0` | `__aeabi_dmul` | IEEE-754 double multiply (exponent add, 53-bit mantissa, `0x7ff`/`0x3ff`). |
| `0x7d10` | `__aeabi_d2f` | double→float (rebias `0x380` = 1023−127). |
| `0x7f9c` | `dmul_special_operands` | libgcc dmul NaN/Inf/denormal helper (called only by `0x7dc0`). |
| `0x336c` | `prvUnlockQueue` | walks both queue waiter lists (`+0x24`/`+0x10`) waking tasks. |

**Flagged candidate-vendor (not renamed — pending exact-symbol confirmation):** the
FreeRTOS task-notification cluster `0x6c70`/`0x6dc4`/`0x74e0` (notify-give / ready-
list insert / notify-wait, called by `commport_queue_receive`+`xQueueGenericSend`),
the ISR-safe queue send `0x9652`, and the software-timer create/command pair
`0x15e0`/`0x6fd0` (the latter borderline — its cmd-6 path calls the VanMoof
`commport_rx_complete_cb`; flagged per "flag, don't guess").

**Reconciled:** `comm.c` dropped the `FUN_0000925a` extern (now `commq_bytes_used`,
2 call sites) and renamed the `FUN_00003338` extern → `vQueueDelete`; `comm.h` got
the `commq_bytes_used` prototype. `comm_txisr.c` extern `FUN_00006bfc` →
`xTaskRemoveFromEventList` + `rtos_sem_give`. `util.{c,h}` gained `vmem_strncmp`.
`store.{c,h}` gained `xfer_state_log_notify` + the waiter/state globals. `main.c`
reconciled `func_0x00009876`→`vmem_strncmp` (now via `util.h`, cast at the call),
`func_0x00001884`→`xfer_state_log_notify`, `FUN_00003cd0`→`vQueueAddToRegistry`,
`FUN_00003338`→`vQueueDelete` (decompiler-form local externs kept ABI-compatible).
Ghidra renamed + saved (15 functions); plate comments on `0x3338`/`0x3cd0`.

## Decompiled — batch 22 (`src/`, `include/`) — comm-port TX/ISR path (worklist 2c)

The CAN/comm-port **TX engine + ISR / eDMA-ring data path**, in new module
`comm_txisr.c` (kept separate from `comm.c` for readability). One missed function
was **carved** first (`commport_fifo_isr` @0x2510, `create_function`; the drain
task `0x3c88/0x3c90`, alt callback `0x3110`, and fused TX task `0x4be8` were left
**un-carved** — interior/fused/ambiguous, deliberately not forced). The 6
functions were translated via an analyze→adversarial-verify workflow (12 agents),
which **caught real bugs** (a latent infinite loop in the FIFO ISR loop-reset; a
swapped chain-TCD arg order in the TX engine; dropped return/args). All 20
modules compile clean.

| Module | Function | Verdict / note |
| --- | --- | --- |
| comm_txisr | `commport_fifo_isr` (`0x2510`) | **faithful** (verify-corrected loop reset). FIFO PIO pump (push +0xe20 / pop +0xe30, throttled by +0xe04/+0xe08); on completion latches `0x15e1` and tail-calls the completion callback. Uses a `commport_xfer_t` transfer descriptor. |
| comm_txisr | `commport_irq_dispatch_inst3` (`0x2190`) | **faithful**. `idx=irqn_to_gpio_index(0x40089000)`; tail-calls `cb_tbl[idx](base_tbl[idx], handle_tbl[idx])`. One of 9 byte-identical vector trampolines. |
| comm | `commport_isr_install` (`0x78f0`) | **faithful**. Zeroes a 0x30-byte handle, binds handle/callback into the per-instance tables (CAN-mode-selected cb `0x3111`/`0x96e1`), seeds FIFO levels, enables NVIC. (Assembled in `comm.c`.) |
| comm_txisr | `commport_rx_complete_cb` (`0x96ce`) | **faithful**. Under BASEPRI: push the RX frame into the SW ring, maintain a signed seq counter, fire a FreeRTOS task-notify at the -1 sentinel. |
| comm_txisr | `commport_ring_drain` (`0x2d34`) | **faithful**. Per descriptor: clamp burst length, rebuild an eDMA TCD (`edma_tcd_build`), toggle flip, advance cursor; stops at 2 TCDs in flight. |
| comm_txisr | `commport_can_transmit` (`0x7624`) | **faithful** (verify-corrected chain-TCD arg order). The TX/enqueue engine: enqueue {buf,len}, prime HW (primary + 2 chain TCDs, FIFO dir at +0xe00, arm eDMA, kick TX at +0xc00), then `commport_ring_drain`. medium confidence (largest). |

**Reconciled:** `comm.h` got the `commport_isr_install` prototype; the carved
`0x8e85`/`0x3110`/`0x93cf` callback pointers stay fn-ptr constants (the `0x8e85`
one echoes the registry anomaly — left as-is). The `0x2190` callback table and
the `0x78f0` install tables are the same SRAM tables (`0x200016a0`/`0x200016c4`).
`main.c` name-reconciled `FUN_000078f0`→`commport_isr_install`. Cross-references
between the two `edma_tcd_build`/`edma_chan_irq_enable`/`commport_ring_drain`
signatures bridged with casts (the job/handle/req are different views of the same
object). The eDMA channel descriptor / SW-ring / job structs are modeled as typed
partials in `comm_txisr.c`.

## Decompiled — batches 20–21 (`src/`, `include/`) — comm-port / CAN (worklist 2c)

Batch 20 = the comm-port/CAN **discovery** (4-scout + synthesis workflow; see the
*CAN comm port* subsystem map below). Batch 21 = the **carved tractable** comm-port
driver functions, translated via an analyze→adversarial-verify workflow (9
functions, 18 agents) then assembled into the new module `comm.{c,h}`. All
faithful; all 19 modules compile clean.

| Module | Function | Verdict / note |
| --- | --- | --- |
| comm | `commport_base_to_index` (`0x2c20`) | **faithful**. Base → instance index 0..3 (family `0x40086000` step `0x1000`; `0x89000`→3 = CAN-FD). Leaf. |
| comm | `commport_registry_index` (`0x24f4`) | **faithful**. Linear search of the 8-slot key registry (`0xa558`); returns 0..7 or 8. Verify corrected the dropped return value. |
| comm | `edma_tcd_build` (`0x8a7a`) | **faithful**. eDMA TCD builder: elem-size from `cfg[9:8]`, validates address alignment, writes 4 words (the data-path proof). Leaf. |
| comm | `edma_chan_irq_enable` (`0x8c46`) | **faithful**. RMW-set the eDMA channel IRQ-enable bit at `edma_base+0x50[ch>>5]`. Leaf. |
| comm | `commport_uart_config` (`0x2754`) | **faithful**. LPUART SBR/CTRL/FORMAT/WATER config; calls `commport_registry_index`. (cfg struct padding fixed so `f0c`@`+0xc`.) |
| comm | `peripheral_clock_mux_select` (`0x22b0`) | **faithful** (verify hand-decoded past a truncated disasm). PCC gate + source select at `periph_base+0xff8`; `0/1/3`. Borderline SDK glue. |
| comm | ~~`commport_queue_receive` (`0x7550`)~~ | **[BATCH-24 CORRECTION: FreeRTOS `xStreamBufferReceive` — reclassified vendor, C removed.]** Originally read as a length-prefixed payload-queue receive. |
| comm | ~~`queue_ring_copyout` (`0x91ec`)~~ | **[BATCH-24 CORRECTION: FreeRTOS `prvReadBytesFromBuffer` — reclassified vendor, C removed.]** Originally read as a ring copy-out with wrap. |
| comm | `commport_teardown` (`0x79c0`) | **faithful**. Teardown of the instance @ `0x4009d000` (a *distinct* base, not `0x40089000` — reproduced verbatim): queue delete, event-list free, SW-reset + clock-gate. |

**Reconciled:** unified the per-function struct views into `comm.h`
(`commport_handle_t`, `commq_t`, `edma_tcd_t`, `edma_chan_desc_t`,
`commport_uart_cfg_t`); renamed the agent's colliding `bus_page_write_verify`
context away (n/a here); `main.c` name-renamed `FUN_000079c0`→`commport_teardown`,
`FUN_000022b0`→`peripheral_clock_mux_select`, `func_0x00002754`→
`commport_uart_config`. Not-yet-translated VanMoof helpers (`FUN_0000925a`,
`FUN_00006dc4`, `FUN_000074e0`, `FUN_00003338`, `FUN_00006c70`) + FreeRTOS
(`vTaskSuspendAll`/`xTaskResumeAll`/`vPortFree`/critical) left `extern`; off-image
tables `pcc_gate_arg_table`/`port_clk_arg_table` + globals `g_commport_registry`/
`g_commport_active` declared `extern`.

## Decompiled — batch 19 (`src/`, `include/`) — FOTA updater backend (worklist 2b)

The external-flash FOTA write/verify/descriptor/log chain, built via an
**analyze → adversarial-verify workflow** (8 functions, 16 agents) then assembled
and reconciled by hand. All faithful (each verified against the disassembly).
Lives in `store.{c,h}` alongside the page-cache; the one bus primitive
(`bus_page_read`) went to `bus.c`. All 18 modules compile clean.

| Module | Function | Verdict / note |
| --- | --- | --- |
| bus | `bus_page_read` (`0x29b4`) | **faithful**. Variant-dispatched page read `(a,addr,buf,len)` → 3 fixed off-image handlers (`0x130043a2`/`0x13007538`/`0x1300ade4`), variant B sub-selected by `(*0x40000ffc & 0xf)`. Verify corrected the decompiler: 4-arg forward + result (not 1-arg void). |
| store | `event_report` (`0x3eac`) | **faithful**. Builds a 30-byte record `{ctx@0, code@4, payload[n]@6}` and posts it to the manager's event queue (`mgr+0x590`) via `xQueueGenericSend` (vendor). **Variadic** by word-count (OEM caps at 6 by buffer size, no bounds check). |
| store | `flash_page_write` (`0x442c`) | **faithful** (renamed from the agent's colliding `bus_page_write_verify`). token→program→read-back verify (variant A fixed op / variant B vtable `+0x14`); codes `0x31/0x38/0x42`. Verify caught + fixed a swapped verify out-param order. |
| store | `flash_page_commit` (`0x668c`) | **faithful**. vtable `page_load` (+0x10) stage → `bus_page_read` → `flash_page_write` @ `0x37400` → token; codes `0x65/0x73`; returns `0/-3/-1`. |
| store | `fota_image_verify` (`0x2acc`) | **faithful**. Streams the staged image (byte `0x1c000`+offset, `0x1b400` bound) page-by-page through the HW checksum engine `0x40095000` (ctrl `0x36`, seed `0xffffffff`, result `+8`); power gates `0x40000220`/`0x40000240`. Returns `0/1/2`. |
| store | `store_descriptor_read` (`0x6708`) | **faithful**. Read the 8-byte descriptor @ `0x37400` (commit-path → vtable `page_load` fallback → `bus_page_read` extract). Reconciled to the translated names + `bus.h` typed vtable (local overlay retired). |
| store | `store_descriptor_write` (`0x6794`) | **faithful**. commit → zeroed page + 8-byte descriptor → `flash_page_write` @ `0x37a00` → re-commit. `-2/-1/0`. |
| store | `log_append_event` (`0x681c`) | **faithful**. Builds an 8-byte record `{src.word0, src.half4, tag=1, flag}` and commits via `store_descriptor_write`, when a log handle is registered. |

**Reconciled:** `bus.h` driver vtable `+0x10` typed as `page_load` (3-arg) and
`+0x14` as `page_verify` (6-arg), retiring the local overlay; `FUN_0000926c` →
`xQueueGenericSend` (vendor). **Off-image externs** (runtime/`0x13xxxxxx`):
`bus_page_read_handler_a/b0/b1`, `bus_verify_op`; runtime RAM globals
`g_log_store_handle` (`0x2000069c`), `g_log_event_src` (`0x0001b31f`), the event
handle slot (`0x2000171c`). The image **apply/boot-swap remains off-image**.

## Subsystem map — FOTA updater + CAN comm port (batch 18 investigation)

Two subsystems located and characterised (high confidence). Only the leaf
`checksum_feed` is translated so far; the rest is mapped for follow-up batches.

### FOTA updater — external-flash image staging backend

An external flash/storage device holds a staged firmware image plus an 8-byte
descriptor; the firmware writes/verifies it, the **off-image bootloader** does
the actual swap (consistent with the unresolved `VTOR=0xc00` boot question).

- **Geometry:** data region byte `0x1c000`..`0x37400` = **`0x1b400` (111 KB)** in
  `0x200`-byte pages (index `0..0xd9`, byte addr `(page+0xe0)*0x200`); the 8-byte
  **descriptor/header sits at `0x37400`** (immediately past the data region).
- **Integrity:** `checksum_feed` (`0x64f0`) streams data into a HW checksum/CRC
  engine at **`0x40095000`** (data reg `+8`); `FUN_00002acc` reads the staged
  image page-by-page through it and returns the result → image verify.
- **Status/error reporting:** `FUN_00003eac` builds a 30-byte message and posts
  it via `FUN_0000926c` (= FreeRTOS `xQueueGenericSend`, **vendor**) to a queue
  at `handle+0x590` — drained by a comms task that transmits over CAN. Error
  codes seen: `'1'/'8'/'B'` (`0x442c`), `'e'/'s'` (`0x668c`), `0xa1` (`0x6708`).

  | Address | Role |
  | --- | --- |
  | `store_flush`/`store_load` (`0x8876`/`0x88d2`) | page-cache (DONE, batch 17) |
  | `bus_page_program` (`0x6610`) | page write dispatch (DONE, batch 16) |
  | `checksum_feed` (`0x64f0`) | HW checksum engine feed (DONE, this batch) |
  | `FUN_00002acc` | read+verify whole image (CRC), GPIO busy at `0x40000220/240` |
  | `FUN_000029b4` | page **read** dispatch (3-way, off-image handlers) |
  | `FUN_0000442c` | page write + read-back verify (+ error report) |
  | `FUN_0000668c` | page write/commit sequence (+ error report) |
  | `FUN_00006708` | read the 8-byte descriptor @ `0x37400` |
  | `FUN_00006794` | write the 8-byte descriptor page |
  | `FUN_0000681c` | append an event/log record (← `FUN_00001884`) |
  | `FUN_00003eac` | event/error reporter → comms queue |

### CAN comm port — unified multi-instance serial IP (batch-20 discovery)

**Hypothesis confirmed but refined** (4-scout discovery workflow): there is **no
classic FlexCAN message-buffer driver**. `0x40089000` (`DAT_00006070`, single
read xref at `main_SystemInit:0x5dda`) is **instance 3 of a 4-member comm-port
family** based at `0x40086000` step `0x1000` (`FUN_00002c20` maps base→index 0..3).
Siblings run as **LPUART** (`FUN_00002754`); instance 3 is configured as
**CAN/CAN-FD** via the bit-timing block at base`+0xc00`. The MCU-side data path is
a **software descriptor ring + eDMA**, not hardware mailboxes.

- **CAN-base registers used** (from `0x40089000`): `+0xc00` CTRL1/bit-timing
  (bit0=TX kick), `+0xc04`/`+0xc1c` secondary timing, `+0xe00` FIFO/transfer ctrl,
  `+0xe08` FIFO status/watermark, `+0xe14` FIFO command (`0xc`), `+0xe20` RX FIFO
  data (eDMA src), `+0xe30` TX FIFO data (eDMA dst), `+0xff8` clock mux. Bit-timing
  from the descriptor at SRAM `0x200070bc` (`DAT_00006028`, ~17 bytes).
- **Separate peripheral — eDMA/DMAMUX at `0x40082000`** (`DAT_00006034`): the
  `+0x400`/`+0x20`/`+0x48`/`+0x50` "MB/IMASK"-looking accesses are eDMA channel
  control, base from `record[4]+8`, channel id `record[4]+0xc`. NVIC ISER
  `0xe000e100`.
- **Driver functions:** TX `commport_can_transmit` (`0x7624`) + `commport_ring_drain`
  (`0x2d34`) + `edma_tcd_build` (`0x8a7a`); ISR `commport_irq_dispatch` (`0x2190`)
  → uncarved FIFO body `0x2510`-`0x274c` → completion cb `commport_rx_complete_cb`
  (`0x96ce`, installed by `commport_isr_install` `0x78f0`); config
  `commport_base_to_index` (`0x2c20`), `commport_current_index` (`0x24f4`),
  `commport_uart_config` (`0x2754`), `peripheral_clock_enable` (`0x22b0`); queue
  helpers `commport_queue_receive` (`0x7550`)/`queue_ring_copyout` (`0x91ec`);
  `edma_chan_irq_enable` (`0x8c46`); `commport_teardown` (`0x79c0`).
- **Two queues:** (A) the 30-byte event/status queue at `devmgr+0x590` (posters
  incl. `event_report`; drain task `0x3c90`, uncarved); (B) a 4-byte TX-trigger
  queue at SRAM `0x200006a0` consumed by the **comm TX task** `0x4be8` (entry
  `DAT_00006054=0x4be9`, ctx `0x20001734`) — fused into `main_SystemInit`, not
  independently carved.
- **Open (dynamic/off-image):** the init-time callback-table write of `0x8e85`
  (interior to `device_read_record87` — the **same registry anomaly**; do NOT
  re-carve `0x8e76`) vs the real `0x96e1`/`0x3111` callbacks installed later by
  `0x78f0` (ordering question); the on-wire CAN ID/DLC mapping lives in the
  off-image upper protocol layer. `hardware.md` should gain `0x40089000` (comm
  port / CAN-FD), `0x40082000` (eDMA), `0x40095000` (checksum).

## Decompiled — batch 17 (`src/`, `include/`)

New module **`store.{c,h}`** — the external-storage **page-cache** that sits on
the `bus_*` dispatch shims (batches 10/15/16). Both faithful (every field offset
and the page-address math verified against the disassembly); compiles clean.

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| store | `src/store.c`, `include/store.h` | `store_flush` (`0x8876`) | **faithful**. If a page is cached (`+0x44 != 0xffff`): `bus_page_program(st, (page+0xe0)*0x200, st->buf@+0x40)`; then count `+0x46 <= 1` → invalidate, else clear buffer + advance page + decrement count. Returns the program status. The OEM's combined 32-bit `0xffff` store at `+0x44` (page=0xffff, count=0) is reproduced as two field writes. |
| store | (same) | `store_load` (`0x88d2`) | **faithful**. `store = ctrl+0x18`; clears `ctrl+0x10/+0x15`; requires `store->enable@+0x3c` and `range.start <= 0xd9`; `store_flush`; clamps count so the window ends at page `0xd9`; reads each page with `bus_transfer_token(store, (start+0xe0)*0x200 + i*0x200)`; latches `[start, count)` at `+0x44/+0x46`; sets `ctrl+0x06=start`, `ctrl+0x08=0`. Args 2/4 are the OEM's unused r1/r3. |

**Storage geometry:** 0x200-byte pages, index `0..0xd9`, byte address
`(page + 0xe0) * 0x200`. `store` object: `+0x3c` enable, `+0x40` page buffer,
`+0x44` cached-page idx (`0xffff` = invalid), `+0x46` count. Modeled as opaque
partial structs in `store.c` (only the touched fields). No source callers yet
(`store_flush` ← `0x2acc`; `store_load` ← only itself in the current image).

## Decompiled — batch 15 (`src/`, `include/`)

| Module | File | Function | Verdict / note |
| --- | --- | --- | --- |
| bus | `src/bus.c`, `include/bus.h` | `bus_transfer_token` (`0x664c`) | **faithful** dispatch; **operation semantics unconfirmed**. Variant A tail-calls a fixed off-image handler (`0x1300413a`, `DAT_00006680`); variant B tail-calls driver vtable `+0x08`. Both get `(a, b, 0x200, 0x6b65666c)`. The variant-A handler is modeled as an `extern` (off-image), the vtable `+0x08` slot typed in `bus.h` — so no `void*`→fn-ptr casts. Callers: `main_SystemInit` + `0x442c`/`0x668c`/`0x88d2`. `main.c` name-reconciled. |
| bus | (same) | `bus_page_program` (`0x6610`) | **faithful** dispatch; the write-back half of the storage page-cache. Variant A → fixed handler `0x1300419c` (`DAT_00006644`); variant B → driver vtable `+0x0c`. Both get `(sess, addr, buf, 0x200)`. Modeled like `bus_transfer_token`. Caller: `0x8876` (cache flush, not yet translated). |

### Driver-vtable map (`*(g_bus_mgr+0x10)`), consolidated

| Offset | Variant-B slot | Variant-A handler | Dispatcher |
| --- | --- | --- | --- |
| `+0x04` | init | — | `bus_session_init` |
| `+0x08` | xfer_token | `0x1300413a` | `bus_transfer_token` |
| `+0x0c` | page_program | `0x1300419c` | `bus_page_program` |
| `+0x1c`/`+0x28` | open A/B | — | `bus_session_open` |
| `+0x24`/`+0x30` | write A/B | — | `bus_write_commit` |
| `+0x2c`/`+0x38` | xfer A/B | — | `bus_transfer` |
| `+0x40`/`+0x4c` | probe A/B | — | `bus_probe_read` |

Still-pending dispatcher: `FUN_000029b4` (3-way, fixed handlers
`0x130043a2`/`0x13007538`/`0x1300ade4`, selected by variant + `(*0x40000ffc & 0xf)`).

## Reclassified vendor — batch 14 (no C)

The **FreeRTOS queue/list/semaphore creation** cluster, identified while scanning
`main.c`'s remaining `FUN_*` externs. Named in Ghidra for understanding, counted
`deferred (vendor)`, never reconstructed (satisfied by upstream FreeRTOS):

| Address | Name | Evidence |
| --- | --- | --- |
| `0x95be` | `xQueueGenericCreate` | `pvPortMalloc(count*size + 0x48)` (Queue_t header), the `count != (count*size)/size` multiply-overflow `configASSERT`, sets pcHead/pcTail/uxLength/uxItemSize, two `vListInitialise` for the waiting lists. |
| `0x8c8a` | `vListInitialise` | `pxIndex = &xListEnd`; `xListEnd.xItemValue = 0xffffffff` (portMAX_DELAY); `xListEnd.pxNext = pxPrevious = &xListEnd`; `uxNumberOfItems = 0`. |
| `0x974e` | `xQueueCreateMutex` | `xQueueGenericCreate(1, 0)` (a semaphore-shaped queue) then zeroes the holder fields and `rtos_sem_give`s it (mutex starts available). |
| `0x879c` | `xSemaphoreCreateBinary` | bare tail-call `xQueueGenericCreate(1, 0)` (binary semaphore, starts empty). |

`main.c` name-reconciled (incl. `thunk_FUN_0000974e`→`thunk_xQueueCreateMutex`).
Usage there confirms the roles: data queues `(0x80,0x10)`/`(4,7)`, semaphores
`(1,0)`, mutexes, binary sems.

## Orphaned-gap sweep (read-only) — missed VanMoof code located

A read-only classification of **all 18 orphaned-instruction code gaps** that sit
between named functions (18 scope agents + an adversarial-verify pass; nothing
carved, nothing mutated). Result: the static frontier is **not** exhausted —
**~16 genuinely-missed VanMoof functions** live in 5 gaps Ghidra never carved, and
several prior assumptions are corrected.

**Carvable VanMoof — verifier-CONFIRMED (translation targets):**

| Gap | Entry(s) | What it is | Caller proof |
| --- | --- | --- | --- |
| `0x1908-0x1a33` | `0x1914`, `0x193c`, `0x19f8` | 3 xfer/commport fns: flag-gated waiter reset; critical-section framed-message poster (`bfi` id, `rtos_sem_give`); head−tail buffer fill-level | C@`0x19f8` ← real `bl 0x3f86`; A/B via runtime dispatch |
| `0x29ea-0x2acb` | `0x29f8` | flash/store **page write+verify** — sibling of `fota_image_verify` (+0x3c/+0x40/+0x44 fields, 0x200 page, 0xd9 sector; calls `vmem_set`/`bus_page_read`/`store_flush`/`vmem_copy`) | indirect (off-image `0x13xxxxxx`, exactly like `fota_image_verify`) |
| `0x3cf0-0x3eab` | `0x3cf4 0x3d14 0x3d48 0x3d80 0x3dc8 0x3ddc` | ~6 event/notify helpers + a **GPIO button poller** (base `0x4008c000`, debounce via `0x15e0` w/ 1000/200 ms) — siblings of `event_report`, push to the `+0x590` stream buffer | `0x3d14` ← `bl 0x9398`, `b.w 0x93b2` (defined code) |
| `0x3f0a-0x442b` | `0x3f14 0x3fe8 0x40d4 0x410c 0x41a4` | ~5 store/registry-iteration + **device-manager dispatch** (`blx r7` callback, 0x4c/0x20-stride tables, BE field packing, periph reset `0x40000000+0x38c`); many `bl event_report` | `0x41a4` ← `b.w 0x93fe/0x9404` |
| `0x2ec8-0x3143` | `0x2ed4` **only** | VanMoof **commport CAN-ring producer** (tail-calls `commport_ring_drain`, same `+0xe00/0xe04/0xe20/0xc00` window) | pattern-match (no static caller, like `fota_image_verify`) — *but `0x3090` in this same gap is FreeRTOS, defer it* |

**Ambiguous (real, coherent fn — but no confirmed caller → possibly dead code):**
`0x8a8` (gap `0x8a2-0xb03`) — a byte-stream parser/state-machine with an `"AGEP"`
magic and two dispatch tables; clean prologue/epilogues, ends exactly at
`q16_exp`. The verifier **refuted** the "registered handler" evidence (the six
"pointer-table" words are `0x8a4` *data* refs to a `1e6` float literal, not
`0x8a5` code pointers) — so it is real but with **no** discoverable inbound `bl`
or function-pointer. Carve for study, but flag (orphan/dead-code candidate; the
jump-table targets at `0xae0` point into the `0x15ae/0x1638` region).

**Corrections to prior notes (the rest of the gaps — all VENDOR / data / interior):**
- **`0x3c90` "drain task"** (gap `0x38d0-0x3ccf`) → **FreeRTOS** `xQueueSemaphoreTake`(100-tick)+64-bit event-bit-clear helper. **NOT a VanMoof task.** (Was the worklist-1 "deferred drain task" candidate — withdrawn.) The gap is `rtos_sem_take`'s literal pool + FreeRTOS object-create wrappers.
- **`0x1c10`** (gap `0x1c0c-0x1e1b`) → **vendor SDK-HAL** clock/NVIC bring-up orphan (verbatim MMIO to `0x4008C000`/`0x40006000`/`0x40020000`/`0x40000000`, `blx r3` via dispatch table; zero callers). (Was flagged a "strong VanMoof candidate" — withdrawn.)
- **`0xe10-0xf97`** → **interior tail of `ledEasing_ControlUpdate`** (q16 cold paths; entered by conditional jumps from `0xca8/0xcbe`, branches back to `0xc7c/0xcae/0xd26`). This also confirms **`sensors_task`@`0xf98` is a fall-through continuation of that body**, not a real task entry → reinforces worklist 1. (`0xd96-0xe01` is the same tail.)
- **`FUN_000015e0` + `FUN_00006fd0`** → both **FreeRTOS software-timer** internals (timer-create via `pvPortMalloc(0x28)` / timer-list-swap; the full `prvTimerTask`+`prvProcessExpiredTimer` daemon is the `0x7012-0x7287` gap). **Resolves 2 of the 3 "ambiguous timer/clock" residuals.**
- Remaining gaps: `0x16b8-0x1883` / `0x1a9a-0x1bdb` → FreeRTOS timers.c; `0x9386-0x9509` → FreeRTOS stream_buffer.c (matches the stream-buffer-is-vendor note); `0x7c2e-0x7d0f` → libgcc soft-float (interior of `__aeabi_f2d`); `0x83a-0x883` / `0x1034-0x110b` → literal pools + trap padding; `0x22be-0x24f3` → `peripheral_clock_mux_select` tail (extend-prev-fn) followed by a vendor PCC clock-gate/callback-dispatch family.

> **Net:** carving the 5 confirmed gaps adds ~16 VanMoof functions across the
> commport/xfer, flash/store, event/notify+GPIO-input, and store/registry+device-
> manager-dispatch domains. All entries had verified prologues + literal-pool
> boundaries; carving was `create_function` at the listed addresses, all
> **non-overlapping**. **DONE in batch 26** (below). `0x8a8` was carved, found to
> resolve into a 3.6 KB computed-jump hull overlapping `q16_exp`/`ledEasing`/etc.
> (entangled, no caller) and **reverted** — left un-reconstructed (needs dynamic).

## Decompiled — batch 26 (`src/`, `include/`) — orphaned-gap carve + translate

The 5 confirmed orphaned gaps above, carved (`create_function`) and translated via
an analyze→adversarial-verify workflow (16 functions × 2 agents). The adversarial
pass **caught real bugs** before integration: a wrong `bus_transfer_token` arg in
`fota_image_write` (OEM passes `vmem_copy`'s return `buf+0x1fc`, not the handle —
`vmem_copy` proven to return dst in r0, so its decl is now `void *`); three
array-stride double-counts in `commport_tx_complete_advance` (slot index is
`head`, not `head*2`); a dropped `int` tail-return in `event_notify_post_state`;
and three off-by-one timer-handle derefs in `button_scan_poll` (pass `entry->timer`,
not `entry`/`&entry->timer`). All **25 modules** compile clean (`-Wall -Wextra
-Wpedantic -Wshadow`, zero warnings).

| Module | Functions (OEM addr) | Note |
| --- | --- | --- |
| store.c | `fota_image_write` (0x29f8), `xfer_waiter_notify` (0x1914), `xfer_waiter_post_frame` (0x193c), `timer_remaining_ticks` (0x19f8) | FOTA footer page write+verify (sibling of `fota_image_verify`); xfer waiter reset/post-frame; FreeRTOS-timer "expiry − xTickCount" query (custom wrapper, VanMoof). |
| comm_txisr.c | `commport_tx_complete_advance` (0x2ed4) | eDMA TX-complete ring advance + RX-FIFO flush + channel re-arm; tail-calls `commport_ring_drain`. New `commport_tx_ring_t`/`commport_tx_chan_t`/`commport_q_slot_t` views. |
| event.c **(new)** | `event_notify_post` (0x3cf4), `input_event_post` (0x3d14), `event_notify_post_state` (0x3d48), `xfer_state_lock_post` (0x3d80), `event_post_boot` (0x3dc8), `button_scan_poll` (0x3ddc) | stream-buffer event posters (mgr+0x590) + GPIO button-scan/debounce poller (base 0x4008c000). |
| device.c | `device_mgr_reset` (0x3f14), `device_apply_task` (0x3fe8), `device_fetch_cache_status9c0` (0x410c), `device_dispatch_command` (0x41a4) | device-block re-arm; never-returning command-queue drain task; status fetch+cache; inbound-command dispatch + fragment reassembly. |

**`vmem_copy` ABI corrected (util.{c,h}):** `void` → `void *vmem_copy(...)` returning
`dst`. The OEM walks via `r3 = dst-1` and never touches `r0`, so dst survives in r0
as the return value (the memcpy contract); `fota_image_write` relies on this to
forward `buf+0x1fc` into `bus_transfer_token`. Existing call sites are unaffected.

**`i2c_reg_write_53` (0x1a34) `void` → `int`:** the OEM tail-returns the
`iom_i2c_transfer` status, which `device_mgr_reset` checks. (i2c.{c,h}.)

> **Process note (not for any published surface):** the translation workflow's
> subagents wrote the src/ files directly (concurrently), which raced — only 8 of
> 16 functions landed, `0x19f8` was referenced under two non-matching extern names,
> and the 4 adversarial corrections weren't applied. Reconciled by hand from the
> verified workflow output: added the 8 missing functions, applied all 4
> corrections, unified the `0x19f8` symbol to `timer_remaining_ticks`, and rebuilt.
> The device.c agent's "0x19f8 = vendor" label was wrong (the sweep classified it
> carvable VanMoof; it is a custom timer wrapper) — reconstructed as VanMoof.

## Decompiled — batch 27 (`src/`, `include/`) — final `FUN_*` frontier sweep

Classified **every** remaining `FUN_*` (12) in Ghidra plus the one named-but-no-C
function (`int_pair_to_float`). Outcome: the VanMoof static frontier is exhausted —
only **2 genuine VanMoof leaves** remained; the rest is FreeRTOS vendor or flagged.

| Module | Function (OEM addr) | Note |
| --- | --- | --- |
| control.c | `int_pair_to_float` (0x884) | VFP `{whole, micros}` int-pair → float: `(float)p[0] + (float)p[1]/1e6f` (divisor literal `0x49742400` @0x8a4). 2 call sites in the control task (0x520/0x52e, inside the vendor-extern `0x370`). **Instruction-byte-exact** by objdump (2×`vcvt.f32.s32`, `vdiv`, `vadd`). |
| pcc.c | `clockgate_status_ack` (0x84be) | Clock-gate block (0x40004000) status-ack helper: returns `block[0x28]>>24` (pending byte) and, if set, W1C-rewrites `block[0x2c]`. Called from the clock-gate IRQ trampolines (0x2314/0x2344) which tail into `pcc_gate_set`. **Byte-for-byte identical** to the OEM. |

**Remaining `FUN_*` — FreeRTOS vendor (renamed in Ghidra, `deferred`, never reconstructed):**

| Address | Symbol | Fingerprint |
| --- | --- | --- |
| `0x33f0` | `xQueueReceive` | `prvCopyDataFromQueue` + `xTaskCheckForTimeOut` + `vTaskPlaceOnEventList`/`prvUnlockQueue` loop. |
| `0x6330` | `prvAddCurrentTaskToDelayedList` | inserts the task into one of the two delayed-task lists (overflow-aware) via `vListInsert`. |
| `0x7018` | `prvTimerTask` | software-timer daemon: pops the active-timer list, fires `(*cb)(timer)`, reloads auto-reload timers via `0x6fd0`. |
| `0x642c` | `prvTaskExitError` | `configASSERT(uxCriticalNesting == ~0)` then infinite spin — the address tasks return into. |

Already-documented vendor still `FUN_*` (rename pending exact symbol): `0x15e0`
xTimerCreate, `0x6dc4` task-notify wait, `0x6e20` queue-receive internal, `0x6fd0`
timer-cmd dispatch/list-swap, `0x74e0` task-notify give.

**Flagged — do NOT reconstruct (Ghidra plate comments set):**
- `0x9408` — stream-buffer receive + embedded-callback pump (reads a `{code* fn,
  void* arg, int flag, payload[132]}` message via `xStreamBufferReceive` /
  `prvReadBytesFromBuffer`, then `fn(arg, payload)` when len>0xb). **Zero static
  xrefs**; sits inside the stream_buffer.c vendor cluster. Likely a vendor
  stream/message-buffer receive variant, or an unanchored VanMoof work-dispatcher
  whose caller is off-image — insufficient evidence to claim VanMoof, so flagged.
- `0x9544` — Ghidra reports bad-instruction data; body is a `configASSERT`-style
  halt adjacent to the queue/stream cluster. Suspected vendor trap / mis-carve.

## Worklist (prioritized)

1. ~~**Re-carve task bodies.**~~ **INVESTIGATED — NOT carvable (anomaly).** The
   `xTaskCreate` `pxTaskCode` entries all point into the **interior of existing
   functions**, not real prologues: `l_led_ring_task`=`DAT_00005970`=**0x473d**
   and `r_led_ring_task`=`DAT_00005978`=**0x4769** land in `main_SystemInit`'s
   NVIC-enable sweep; `sensors_task`=**0x0f99** lands in the control/easing
   region (`ldr r3,[sp]` before a `q16_div`, no prologue — Ghidra's 0xe02-0x1033
   "sensors_task" is mis-bounded interior code); `dmic_task` entry is off-image
   (0x1b3xx). The `pcName` args correctly resolve off-image (0x1b2ff..0x1b341),
   confirming the arg positions — so the real task bodies are almost certainly
   **off-image** (upper ROM past 0x1a88b) or runtime-patched. Carving any of these
   would overlap existing code (the [[registry-comparator-anomaly]] trap). **Do
   NOT `create_function` here** — needs the off-image ROM (item 3) + dynamic
   analysis. (Confirmed by the orphaned-gap sweep: `0xe10-0xf97` — incl. the
   `0xf98` entry — is the cold tail of `ledEasing_ControlUpdate`, not a task body.)
   The *separate* orphaned-instruction gaps are now classified — see **Orphaned-gap
   sweep** above: 5 carvable VanMoof gaps; `0x38d0-0x3ccf` (drain `0x3c90`) and
   `0x1c0c-0x1e1b` turned out **vendor**, not missed VanMoof.
2. ~~**I²C device/bus cluster:** device-read family (batch 9), bus-session layer
   (batch 10), device write/apply (batch 11), device store/command siblings
   (batches 12–13), bus dispatch shims (batches 15–16).~~ **DONE end-to-end**
   (`registry.*` → `device.*` → `bus.*`). The `cmp=0x8eb4` puzzle remains a
   separate **dynamic-analysis** open item.

2a. ~~**External-storage page-cache** core — `store_flush` (`0x8876`),
   `store_load` (`0x88d2`).~~ **DONE (batch 17)** → `store.{c,h}`. Remaining in
   this layer:
   - `FUN_0000668c` (page write sequence): driver vtable `+0x10` method →
     `FUN_000029b4` → `FUN_0000442c` → `bus_transfer_token`, with `FUN_00003eac`
     error reporting (`DAT_00006700` buffer, `DAT_00006704` error ctx).
   - Dispatch primitive `FUN_000029b4` (3-way fixed-handler: `0x130043a2` /
     `0x13007538` / `0x1300ade4`, by variant + `(*0x40000ffc & 0xf)`).
   - `FUN_0000442c`, `FUN_00003eac` (frame builder/error report) still `extern`.
   - `FUN_00002acc` is the image read+verify (CRC) — see the FOTA section.

2b. ~~**FOTA updater backend** — storage page R/W/verify/descriptor/log chain
   (`0x2acc`/`0x442c`/`0x668c`/`0x6708`/`0x6794`/`0x681c`/`0x29b4`/`0x3eac`).~~
   **DONE (batch 19)** → `store.{c,h}` + `bus_page_read`. The image
   **apply/boot-swap remains off-image** (bootloader, ties to item 4). Residual
   leaf still `extern`: the verify op handlers and `port_clock_wait` (done).

2c. **Comm-port / CAN driver** — **MAPPED (batch-20 discovery)**; see *CAN comm
   port* above. Not a FlexCAN MB driver: a 4-instance eDMA+FIFO serial IP, CAN-FD
   on instance 3 (`0x40089000`). Translation plan:
   - ~~**Carved & tractable** (9 functions: index/registry/eDMA-TCD/IRQ-enable/
     uart-config/clock-mux/queue-receive/ring-copyout/teardown).~~ **DONE (batch
     21)** → `comm.{c,h}`.
   - ~~**TX/ISR path** (`0x7624` transmit, `0x2d34` drain, `0x2190` dispatch,
     `0x78f0` isr-install, `0x96ce` rx-cb) + carved `commport_fifo_isr`
     (`0x2510`).~~ **DONE (batch 22)** → `comm_txisr.c` (+ `commport_isr_install`
     in `comm.c`).
   - ~~**Frame/ring tail** (`0x85f8`, `0x976e`).~~ **DONE (batch 23)** →
     `commport_frame_enqueue`/`commport_dma_ring_read_frame`. (`0x925a` was also
     done in batch 23 as `commq_bytes_used` but **reclassified FreeRTOS
     `prvBytesInBuffer` vendor in batch 24 — C removed**.) The rest of `comm.c`'s
     old helper-extern list is vendor (`vQueueDelete` `0x3338`; FreeRTOS
     stream-buffer `0x7550`/`0x91ec`/`0x925a`/`0x86dc`/`0x86fa`/`0x8754`/`0x926c`;
     notify cluster `0x6dc4`/`0x74e0`/`0x6c70`/`0x744c`).
   - ~~**Frame encode + dispatch** (`0x84f6`, `0x8852`).~~ **DONE (batch 24)** →
     `commport_frame_encode_dispatch`/`event_handler_dispatch` in `comm_txisr.c`.
   - **Still un-carved (deferred — interior/fused/ambiguous):** the drain task
     `0x3c88`/`0x3c90` (entry-ptr `0x3c91` vs prologue `0x3c88` discrepancy), the
     alt RX callback `0x3110` (interior code, callback ptr `0x3111`), and the comm
     TX task `0x4be8` (fused into `main_SystemInit`). Each needs a confirmed
     boundary before carving. The `0x8e85` callback-table write stays the open
     **dynamic-analysis** registry-anomaly thread.

3. **Load the off-image ROM region (`>0x1a88b`):** resolves the xTaskCreate name
   strings (`0x1b2f2..0x1b341`) and the CRC-8 LUT (`0x1b364`).
4. **Boot entry / live vector table:** find how the CPU reaches `0x1d4` and the
   true contents at `VTOR=0xc00` (look for a separate first-stage bootloader).
5. **Confirm the NXP SKU** by matching the absolute peripheral base map to vendor
   datasheets (S32K144 refuted); reconcile with the Ambiq-IOM comms read.
6. **Decompile the PWM and SAI/DMIC driver bodies** in `main_SystemInit`
   (`0x49a6-0x4b02` PWM, `0x5b00-0x5f3c` SAI/DMA) → classify IP, find LED frame
   buffers / DMIC sample+RMS logic.
7. **Map app-header high-bytes** (`0x19`/`0x0e`/`0x82`/`0x12`) to manifest peer
   ECUs by tracing which I²C addresses each is sent to.
8. Confirm `SystemCoreClock` RAM addr (`0x20001654`) feeds SysTick RVR (1 kHz).
9. Identify the I²C sensor parts by address (`0xf6/0xe0/0xfd` = Sensirion-like;
   `0x44/0x89/0x94` = ?).

## RTOS tasks (from surviving strings)

⚠️ **The "Entry" addresses below are NOT real function bodies** — every
`pxTaskCode` resolves into interior code of an existing function (see worklist 1).
Listed for reference (the off-image `pcName` strings are real); the task bodies
themselves are off-image / not statically carvable.

| Task name | `pxTaskCode` const | Points into | Role (from name) |
| --- | --- | --- | --- |
| `l_led_ring_task` | `0x473d` | `main_SystemInit` NVIC sweep (interior) | left LED ring (PWM) |
| `r_led_ring_task` | `0x4769` | `main_SystemInit` NVIC sweep (interior) | right LED ring (PWM) |
| `sensors_task` | `0x0f99` | control/easing region (interior, pre-`q16_div`) | sensor sampling (ADC + I²C) |
| `dmic_task` | off-image `0x1b3xx` | (not in this dump) | digital (PDM/SAI) microphone |

Plus IDLE + helper tasks (8 created total via `xTaskCreate`). `pcName` strings sit
off-image at 0x1b2ff..0x1b341. Resolving the real bodies needs the upper ROM bank.
