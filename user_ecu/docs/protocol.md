# Protocol — user_ecu inter-ECU / peripheral comms

> Verified live in Ghidra (base 0, ARM Cortex-M4F). All addresses base-0 hex.
> Survey + 3-verifier consensus; status CONFIRMED unless marked TBC.

## Bus / physical layer
- **I²C master via an Ambiq Apollo-class IOM (I/O Master).** Single transfer
  engine: `iom_i2c_transfer` (`0x7288`).
  - Loads the IOM register base from the runtime handle (`ldr r12,[handle],#0x20`),
    block-copies a 7-word transfer descriptor into the handle, then programs the
    IOM command window: `*(base+0x80c)=0x03000051`, `*(base+0x804)=0x50`,
    `*(base+0x808)=0x03000051`.
  - **Blocking / synchronous:** takes an RTOS mutex/semaphore via `rtos_sem_take`
    before submit and gives via `rtos_sem_give` on completion. There is **no
    interrupt-driven RX ring buffer** — the bus ISR only signals the completion
    semaphore the engine waits on.
- Peers are **8-bit I²C device addresses** in the descriptor: `0x89`/`0x94`
  (probe), `0xf6`/`0xe0`/`0xfd` (temp/humidity sensor), `0x55`, `0x44`. All peers
  share one IOM master through the handle at **SRAM `0x200007f4`** (second slot
  `0x200007f8`).

> NB: "Ambiq Apollo IOM" (comms pass) vs "NXP S32K-like" (hardware pass) is an
> unreconciled vendor tension — see `hardware.md` open items. The bus = I²C master
> + the framing below are independent of which is right.

## Framing (software layer over I²C)
- A transaction buffer = a **big-endian 16-bit header word** followed by payload
  words, where **each 16-bit word is immediately followed by a 1-byte CRC-8** —
  i.e. 3-byte groups `{hi, lo, crc}` (per-word CRC, not a single trailing frame
  CRC).
- **TX append word:** `frame_append_word_crc(buf, off, word)` writes `buf[off]=
  word>>8`, `buf[off+1]=word&0xff`, `buf[off+2]=CRC8({hi,lo})`, returns `off+3`.
- **TX submit:** `i2c_tx_frame(buf, nbytes)` builds a descriptor with opcode
  **0x59** (write), length `nbytes-1`, then calls `iom_i2c_transfer`.
- **RX / deframe:** `i2c_rx_frame_verify(buf, nwords)` uses opcode **0x159**
  (read), reads `(nwords>>1)*3` bytes, then de-frames each 3-byte group: recompute
  CRC over `{hi,lo}`, compare to trailing byte (**mismatch → return 1**), compact
  `{hi,lo}` back into the caller buffer.
- Single control write: `i2c_control_write(x)` uses opcode **0x44**.

## Checksum
- **CRC-8, polynomial `0x31`** (Dallas/Maxim "CRC-8/MAXIM"), **first data byte
  inverted**, **no final XOR**, processes one 2-byte word.
  - Bit-banged reference: `crc8_poly31_word` (`0x955e`) — `b = ~data[0];
    8×{ if(b&0x80) b=(b<<1)^0x31 else b<<=1 }; b ^= data[1]; 8×{ same }`. Disasm
    confirms `mvns` (invert first byte) and `eor #0x31`.
  - Table-driven (SMBus PEC form): `crc8_poly31_verify_lut` (`0x6564`) —
    `LUT[ LUT[~b0] ^ b1 ]`, LUT 256-entry @`0x0001b364`. **The LUT is beyond the
    loaded image (~`0x1a88b`), so it is unverifiable from this dump** (poly `0x31`
    is already proven by the bit-banged twin). Doubles as the SMBus PEC validator.

## Command / opcode map (descriptor first half-word → `iom_i2c_transfer`)
Pattern: low byte = command class; bit 8 (`0x100`) toggles read vs write; the
`0x40` family is sensor/register access.
| Opcode | Builder fn | Meaning (proposed) |
|---|---|---|
| 0x53 / 0x153 | `i2c_reg_write_53` / `i2c_reg_read_153` | register write / read (sub-register in payload) |
| 0x44 / 0x144 | `i2c_control_write` / `sensor_read_sht_temp_humidity` | control write / multi-byte register read (6 B; AGEP probe) |
| 0x59 / 0x159 | `i2c_tx_frame` / `i2c_rx_frame_verify` | generic byte-stream write / read |

TBC: `0x40` vs `0x50` family beyond read/write (repeated-start vs stop, or 10-bit
addressing).

## Application-layer message headers (first word built by caller, big-endian)
| Header | Built in | Meaning (proposed) |
|---|---|---|
| 0x1926 | `controlTask_CmdHandler` | control-task setpoint request (params 0x1c/0x1d/0x3a) |
| 0xe28 | `i2c_read_status_e28` | status read (TX 2 B, RX 2 B, byte-swap) |
| 0x8236 | `vPortStartFirstTask_ControlTask` | device init / probe (reads 6 B back) |
| 0x1226 | `vPortStartFirstTask_ControlTask` | setpoint write (appends 0x7fff then 0x6666) |

The high byte (`0x19`/`0x0e`/`0x82`/`0x12`) appears to be a peer/function
selector; the low byte a sub-command. Mapping high-byte → named manifest ECU
(`motor_control`, `elock`, …) is **UNRESOLVED** (logging strings stripped; no name
table in this dump).

## RX vs TX paths
- **TX:** caller builds header+payload with `frame_append_word_crc` (per-word CRC)
  → `i2c_tx_frame`/`i2c_control_write` (opcode 0x59/0x44) → `iom_i2c_transfer`
  (blocks on sem).
- **RX:** `i2c_rx_frame_verify` (opcode 0x159) → `iom_i2c_transfer` (blocks on sem)
  → in-place deframe + per-word CRC verify. Synchronous; no byte ISR.

## Sensors on the bus
`sensor_read_sht_temp_humidity` (`0x1e1c`) reads an I²C temp/humidity sensor
(addr `0xf6`/`0xe0`/`0xfd`), opcode 0x144, then applies **Sensirion-style**
conversion: `T = ((bswap(raw)*0x5573)>>13) - 45000` and
`RH = ((bswap(raw)*0x3d09)>>13) - 6000` (= -45 + 175·S/2¹⁶, -6 + 125·S/2¹⁶).
Family: **Sensirion SHT/SHTC** (verifier-corrected from Si70xx).

## Corrections to first-pass recon (CONFIRMED)
- **"AGEP" (`0x50454741`) is a payload literal, NOT a start-of-frame magic.** Used
  as the probe/ID payload with opcode 0x144 to I²C addr `0x89`/`0x94`. No parser
  branches on it.
- **`0xc8cf5962` (@`0x7bc`) is a logging/module tag, NOT a CRC seed or frame
  magic** (passed to a logger).
- **`0x40080000` is NOT the bus base** — it is the high word of IEEE double `3.0`
  used by soft-float `aeabi_ddiv_softfloat`; the IOM uses the handle's runtime base
  via the +0x800 register window.
- `main_SystemInit` (`0x44c0`) is board + RTOS init, **not** a command dispatcher;
  `vPortStartFirstTask_ControlTask` (`0x370`) is the comms orchestrator.
- The `0x2100..0x22b0` bank + `gpio_irq_dispatch` + `irqn_to_gpio_index` are **GPIO
  interrupt dispatch** (GPIO regs `0x40086000`), unrelated to the data bus.

## Open items (TBC)
- High-byte → peer-ECU mapping (no surviving name table).
- Whether multiple physical I²C segments exist or all peers share one bus (one
  shared handle observed).
- Apollo silicon revision / exact IOM instance (and reconciling with the S32K-like
  clock block).
