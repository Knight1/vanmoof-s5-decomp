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

## Command dispatch & device-level commands

> Layer above the I²C framing (see *Framing*) and the application-layer message
> headers. The device-record registry maps a 3-byte key → a `0x2c`-byte record;
> the same chunked transmitter (`device_send_chunked`) carries both spontaneous
> outbound frames and dispatcher replies. Verified at `0x41a4`/`0x8538`/`0x8656`
> and the accessor band `0x8e0a`–`0x91ce`.

### Inbound command message + dispatch (`device_dispatch_command`, `0x41a4`)
Inbound command frame (byte layout, confirmed `0x41aa`–`0x43e8`):

| Off | Field | Notes |
|---|---|---|
| +0 | key[0] | low byte of 24-bit record key |
| +1 | key[1] | |
| +2 | key[2] | `key24 = msg[0] | msg[1]<<8 | msg[2]<<16` (`0x41aa`) |
| +3 | flags | bit `0x10` = streaming/ack qualifier; bits `0..2` = fragment sub-index |
| +4 | len | payload length |
| +5.. | payload | |

`registry_lookup_value(mgr, key24)` (`0x830a`, called `0x41bc`; only low 3 key
bytes matter) returns the `0x2c`-byte record or NULL → return `0xffffffff`.
Dispatch then branches on **TYPE** (`rec+0x10`) and **MODE** (`rec+0x0f`):

| TYPE @+0x10 | MODE @+0x0f | flags bit 0x10 | Action |
|---|---|---|---|
| 0 single-shot | 0 normal | **must be clear** (`0x4246`; set → `-1`) | run handler, then reply (gate=`flags&0x10`==0) |
| 0 single-shot | 1 raw-only | **must be set** (`0x41de`; clear → `-1`) | run handler, send armed |
| 1 streaming (notify-on-ack) | 0 / 1 | same polarity rule as type 0 | run handler; reply gate=0 (no semaphore) |
| 2 streaming (send-only) | n/a | n/a | send armed; **no reply** (`type!=2` gate at `0x441a`) |
| 0/1 | other (>1) | — | `-1` (`0x41dc`) |
| >2 | — | — | `-1` (`0x41c8`/`0x41cc`) |

The polarity test is `lsls #0x1b` then `bpl`/`bmi` (sign = original bit 4).

**Handler selection** (handler ptr `rec+0x14`; NULL → skip straight to reply,
`0x41e4`). All three paths converge on `handler(ctx=rec+0x18, msg, buf, len)`
(`0x428e`):
- **aux/data path** (type 0, bit `0x10` set, `0x426c`): if `rec->aux` (`+0x20`)
  non-NULL → zero `aux[+0x24]`, copy `msg[4]` payload bytes in, call with
  `buf=aux,len=aux_len`; else zero `rec->data` (`+0x00`, `+0x08` bytes), call with
  `buf=data,len=rec->length`.
- **small-record path** (`rec->length` (`+0x08`) ≤ 8 and `(flags&7)==0`,
  `0x42aa`): stage 8 payload bytes into an 8-byte stack scratch, call with
  `len=msg[4]`, no reassembly.
- **reassembly path** (length > 8, see below).

Handler return `-3` (`0xfffffffd`) short-circuits → return 0, no reply
(`0x4294`).

**Fragment reassembly** — table ptr at `mgr+0x5ac`, 3 slots × `0x4c` bytes
(`dispatch_slot_t`): `age@+0x00` (0 = free), `key[3]@+0x04`, `fill@+0x08`,
`buf[0x40]@+0x0c` (`DISPATCH_BUF_CAP = 0x40`). Write offset =
`(flags&7)<<3` (`0x42d4`).

| Condition | Behaviour |
|---|---|
| sub-index 0, key matches busy slot or free slot found | restart/alloc: `age=1`, `fill=0`, copy key, zero buf (`0x4308`/`0x4358`) |
| sub-index 0, all 3 slots busy | LRU: bump every `age`, evict oldest, **event `0xd8`** w/ evicted key (`0x4392`–`0x43ce`) |
| sub-index ≠ 0, no matching slot | silent drop, return 0 (`0x434c`) |
| sub-index ≠ 0, offset ≠ fill (non-contiguous) | silent drop, return 0 (`0x4304`) |
| `fill + msg[4] > 0x40` | free slot, **event `0xee`**(fill,len,0x40), return 0 (`0x431a`) |
| append, `fill < rec->length` | copy `msg[4]` to `slot+0xc+fill`, `fill += msg[4]`, return 0 (still gathering) |
| append, `fill == rec->length` | call handler with `buf=slot+0xc`, then free slot (`0x43f8`) |

**Reply path** (`0x41ea`–`0x4426`): emitted only when send-gate (r5/`bVar11`==1)
**and** `rec->type != 2`. Build `0xd`-byte frame on stack: zero it, `frame[0..1]=
msg[0..1]`, `frame[2]=msg[2]`, clear seq bit 4 (`bfc #4,#1` on `frame+0x03`).
For type 1: gate=0, no semaphore. Else: take access semaphore `rec+0x04`
(`xQueueSemaphoreTake` `0x8d54`, timeout `0x7fffffff`); on timeout report
**event `0x172`** and return `0xffffffff`; gate=1. Then
`device_send_chunked(mgr+0x594, rec, &frame, gate)` (`0x4416`); for type≠1 release
the semaphore (`rtos_sem_give_dispatch` `0x97f4`). Plain ack (no reply) releases
the type-1 notify semaphore `rec+0x28` via `rtos_sem_give` (`0x6ec0`, `0x4262`).

**Return codes:** `0` on success/buffered/drop/ack/reply; `0xffffffff` on NULL
record, bad TYPE (>2), bad MODE (>1), bit-`0x10` polarity mismatch, or
reply-semaphore timeout (all reach `0x41ce`/`0x4244`).

Diagnostics use `event_report` (`0x3eac`) with subsystem id `DAT_00004428 =
0x312d3f0f`. `device_dispatch_command` itself has only **two callers** (`0x93fe`,
`0x9404`, both inside an uncarved region — the inbound channel is unnamed);
`event_report` by contrast has 40+ call sites image-wide. FreeRTOS sem/queue
primitives are vendor (deferred).

### Device-record command accessors (`0x8e0a`–`0x91ce`)
Each takes the per-record access semaphore (`xQueueSemaphoreTake` `0x8d54`,
timeout `0x64` = 100 ticks) and releases via `rtos_sem_give_dispatch` (`0x97f4`).
Lookup tags are `id | (type<<8)`, byte+2 forced 0 (`registry_lookup_value`
compares low 3 bytes only). The cmd-read pair derive their I²C sub-address from
`bus_page_write_verify` (`0x8b12`), which writes `page_buf+0x30+off` and verifies
read-back from `(off+0x30)&0xff`.

| Fn | Addr | Tag | I²C sub-addr | Record size | Verify | Return |
|---|---|---|---|---|---|---|
| `device_read_record87` | `0x8e76` | `{id,0x87,0x00}` | read `0x30` | 14 B read; cache 13 B (`buf[1..13]`) | reject `buf[0]>1`; opt `vmem_cmp(buf,expect,0xe)` | 0 / -1 |
| `device_read_record91` | `0x8fd6` | `{id,0x91,0x00}` | read `0x40` | 16 B read; cache full 16 B | no length gate; opt `vmem_cmp(buf,expect,0x10)` | 0 / -1 |
| `device_store_field8c0` | `0x8e0a` | `{0xc0,0x08,0x00}` (`0x08c0`) | — (`device_apply`) | store 3 B → `rec->data` | — | void (tail-calls give) |
| `device_store_words8808` | `0x9178` | `{0x08,0x88,0x00}` (`0x8808`) | — (`device_apply`) | store 8 B (2 words) → `rec->data` | — | `device_apply` result; -1 on miss/busy |
| `device_cmd_read87` | `0x8f76` | `{id,0x87}` | write off 0 → `0x30` | frame `0x01 || payload[13]` (14 B); `bus_page_write_verify`→commit→`device_read_record87` | write read-back verify | read result / -1 |
| `device_cmd_read91` | `0x90c0` | `{id,0x91}` | write off `0x10` → `0x40` | 16-B payload (no `0x01` prefix); `bus_page_write_verify`→commit→`device_read_record91` | write read-back verify | read result / -1 |

`bus_transfer` (`0x2910`) is the vendor bus jump-table dispatcher (r2=sub-addr,
r3=len; method `+0x2c`/`+0x38` via `bus_variant_b`); `bus_session_open`
(`0x28c8`) / `bus_session_commit` (`0x2938`, arg 1) / `bus_page_write_verify`
(`0x8b12`) belong to the I²C/bus layer above. The cmd-read pair return -1 if
`bus_session_open` fails. The 3rd register arg of every store/cmd accessor is
the caller's leftover r1/r2 and is never read.

### Outbound command-frame build + chunking
**`device_apply` (`0x8656`)** — single-frame builder. Validates `(mgr,rec)`
non-NULL (else `-2`, `mvn r0,#1`), clears the `0xd`-byte stack frame
(`vmem_set` `0x9866`), stamps the 3-byte key from `rec+0x0c`, then branches on
TYPE `rec+0x10`:

| TYPE | Behaviour | seq bit 0x10 | seq wrap |
|---|---|---|---|
| 0 | single-shot: if `aux_len`(`+0x24`)≠0 set `frame.len`, copy `aux`(`+0x20`)→`frame+5`; call send method once (`0x86ac`) | set (`0x869c`) | n/a |
| 1 | set seq bit, fall into `device_send_chunked(first=1)` | set (`0x86b8`) | mod 8 |
| 2 | `device_send_chunked(first=1)`, no seq bit | not set | mod 16 |
| other | no send, return `0xffffffff` | — | — |

Returns 0 for all three valid types. Outbound frame layout (13 B, confirmed
`0x8662`–`0x8586`): `key[0..2]@+0x00..+0x02`, seq/flags `@+0x03` (bit `0x10` +
counter nibble), `len@+0x04`, `payload[8]@+0x05..+0x0c`.

**`device_send_chunked` (`0x8538`)** — chunked transmitter for types 1/2 and for
dispatcher replies. With `first==0` sends one empty frame (`len=0`, no copy);
else sends `rec->length` (`+0x08`) bytes from `rec->data` (`+0x00`) in
`ceil(len/8)` frames, each `len = min(remaining,8)` (`0x854e`). After each send
the seq nibble at `frame+0x03` is incremented and bitfield-inserted: **mod 16**
(`bfi #0,#4`) when `rec->type==2`, else **mod 8** (`bfi #0,#3`, preserving the
`0x10` flag set by type 1). Transmit channel = `mgr+0x594`, send method ptr at
`channel+0x10` (= `mgr+0x5a4`), called `(*method)(channel, frame)` (`0x856e`).

Call sites of `device_apply` (6, via `get_xrefs_to 0x8656`): inside
`device_store_field8c0` (`0x8e60`), `device_store_words8808` (`0x91de`),
`device_apply_task` (`0x40a4`), `device_fetch_cache_status9c0` (`0x4192`),
`vPortStartFirstTask_ControlTask` (`0x736`), and one uncarved region (`0x3c70`).
Callers of `device_send_chunked`: `device_apply` and `device_dispatch_command`
(reply path).

### Control-task command codes (`controlTask_CmdHandler`, `0x1ee0`)
ABI: `r0=cmd`, `r1=result[]` out, `r2=arg`, `r3=ctx`. 16-byte frame buffer at
`sp+0x8`. On success `result[0]=value`, `result[1]=0` (`strd r3,r4,[r6,#0]` at
`0x1fca`); int return = `0` ok / `4` bad cmd / propagated I²C error. The sensor
path uses
header tag `0x1926` and the per-word CRC append (`frame_append_word_crc`,
`0x899c`); `0x3a` uses tag `0xe28` (see *Application-layer message headers*).

| cmd | Meaning | Returns in `result[0]` |
|---|---|---|
| `0x1c` | sensor read + easing; `count_a=(src0*65535)/100`, `count_b=((src1+45)*65535)/175`; TX tag `0x1926`+2 CRC fields → `i2c_tx_frame`(`0x7408`) → `busyWait_Ticks(0x32)` → `i2c_rx_frame_verify(4)`(`0x73a0`) → `ledEasing_ControlUpdate`(`0xc64`) on both channels | channel **A** quantized output |
| `0x1d` | same path; `(cmd-0x1c)<2` gate matches both | channel **B** output (`res_b`, `sp+0`) |
| `0x3a` | status word via `i2c_read_status_e28`(`0x8964`): tag `0xe28`, TX→`vTaskDelay(0x140)`→`i2c_rx_frame_verify(2)`, byte-swapped reply → `result[0]=frame.h[0]` | status u16 |
| other | early-out, `result[]` untouched | (return value 4) |

Literal pool (`@0x1fd8`, LE): `&g_sensor_src_0=0x20000008`, `65535.0f=
0x477fff00`, `100.0f=0x42c80000`, `&g_sensor_src_1=0x2000000c`, `45.0f=
0x42340000`, `175.0f=0x432f0000`, `g_ctrl_tick_counter=0x2000080c`,
`g_ctrl_struct_A=0x20001ce4`, `g_ctrl_struct_B=0x20000860`. The tick counter is
incremented (sensor path only) before the easing calls (`0x1f94`). Reached from
`vPortStartFirstTask_ControlTask` (`0x69c`/`0x6b6`) and
`device_fetch_cache_status9c0` (`0x411c`).

### Event / status / error records
All posters write one FreeRTOS stream/message buffer at `manager+0x590` via
`xStreamBufferSend` (`0x926c`, vendor — deferred): `r0=queue`, `r1=pos` (record
type/code), `r2=ticks` (carries an object back-ref), `r3=item`, `[sp]=len`. The
manager source varies per poster. Record layouts are **not** uniform:

| Fn | Addr | pos (type) | len | Record | Manager source |
|---|---|---|---|---|---|
| `event_report` | `0x3eac` | `0x4801` | `0x1e` (30) | `{ctx u32@+0, code u16@+4, payload u32[n]@+6}` | handle `*0x2000171c`; queue=`*(*handle+0x590)` |
| `event_notify_post` | `0x3cf4` | `0xa003` | `0xd` (13) | caller payload | `*(src+0xc)` |
| `input_event_post` | `0x3d14` | `0x9a0b` | 3 | `{id,edge,arg}` | `*0x200007f0` (no-op while NULL) |
| `event_notify_post_state` | `0x3d48` | `0x9d19` | 4 | `{0xc0,0x05,0x00,0x01}` | direct arg |
| `xfer_state_lock_post` | `0x3d80` | `0x4795` | 0 | header-only (item=NULL) | `*(lock+0)` |

`event_report` has **no bounds check** on `word_count` vs the 30-byte record
(only 6 u32 fit at +6) — the cap is the caller's responsibility (verbatim OEM).

> The "Manager source" column is shorthand: every poster loads the queue handle
> through one more indirection than shown — e.g. `event_report` resolves
> `manager = *handle` then `queue = *(manager+0x590)`. The `(*slot)+0x590` /
> `*handle` forms above each elide that final load.

Known `event_report` codes / subsystem ids (ctx = record word0):

| code | Meaning | ctx | word_count |
|---|---|---|---|
| `0xd8` | reassembly slot eviction (key bytes) | `0x312d3f0f` | 3 |
| `0xee` | reassembly overflow (fill,len,0x40) | `0x312d3f0f` | 3 |
| `0x172` | reply-semaphore timeout | `0x312d3f0f` | 0 |
| `0x31` ('1') | `flash_page_write` token/select fail | `0xe6384427` | 2 (addr,rc) |
| `0x38` ('8') | `flash_page_write` program fail | `0xe6384427` | 2 |
| `0x42` ('B') | `flash_page_write` verify fail | `0xe6384427` | 2 |
| `0x65` ('e') | `flash_page_commit` scratch read-back fail | `0xe6384427` | 2 (0,rc) |
| `0x73` ('s') | `flash_page_commit` scratch-token fail (returns 0) | `0xe6384427` | 2 |
| `0xa1` | `store_descriptor_read` extract fail | `0xe6384427` | 3 (0,0x37400,status) |
| `0x114` | `record_table_store` index ≥ `0x1f5` OOR | `0x207a5327` | 0 |

`xfer_state_lock_post` (`0x3d80`): NULL obj → `-2`; `lock+0x4 == 2` (already
held) → `-3`; else latch `state(+0x4)=2`, run optional teardown cb `+0x1c`,
post the header-only record, return `sxtb(send_status | cb_status)`. The fixed
lock object `0x20000648` is used by `event_post_boot` (`0x3dc8`, after
`nvic_clockgate_bringup` `0x67f4`).

Connection-state transition (`xfer_state_log_notify`, `0x1884`): `rec+0xa`(u16)
`== 0x3fd` → 'up' (`g_xfer_state_flag` `*0x200070e9` 0→1, `log_append_event(1)`),
any other → 'down'. `log_append_event` (`0x681c`) builds an 8-byte record
`{word0@+0, half4@+4, tag=1@+6, flag@+7}` committed via
`store_descriptor_write` (`0x6794`) — distinct from the queue posters above.

> **Correction to `src/store.c`:** `REC_TABLE_BASE` is written `0x201e1eb4`, but
> the pool literal at `0x4108` is `0x20001eb4`; `record_table_store` (`0x40d4`)
> writes `0x20001eb4 + index*0x2a`. The source constant has a transposed-nibble
> typo.

### CAN comm-port frame command path
On-MCU only: these functions marshal a structured CAN record across the
queue/driver boundary. **The on-wire CAN ID/DLC → peer/command semantic mapping
is off-image** — no in-image xrefs/address-taken refs to any of these (all wired
via registered fn-ptrs / the device-manager ops vtable, registered off-image).

Two on-MCU representations:
- **Packed/on-wire frame:** `[ID3 ID2 ID1 ID0 DLC payload...]`, 4 ID bytes
  big-endian (MSB first), `ID0` (`frame[3]`) masked to 5 bits → 29-bit extended
  id; DLC at `frame[4]`; payload ≤8 B at `frame[5..]`.
- **Internal 16-byte record:** `{dlc@+0 (clamped to 8), pad@+1..3, id32@+4
  (29-bit value, LE word), payload@+8..}`.

| Fn | Addr | Role |
|---|---|---|
| `commport_frame_enqueue` | `0x85f8` | RX decode packed→record. `id=(f[0]<<21)|(f[1]<<13)|(f[2]<<5)|(f[3]&0x1f)`; DLC clamp >7→8; post 16-B msg to FreeRTOS queue (`rtos_sem_give` `0x6ec0`, pos 0 = send-to-back); queue handle = `(*(chan+0x14))[0]`. 0 / -1 |
| `commport_frame_encode_dispatch` | `0x84f6` | TX encode record→packed. id at `frame+4` re-split to 4 BE bytes (`>>21/>>13/>>5/&0x1f`); `rec[4]=frame[0]` (hdr/DLC), copy `frame[0]` payload bytes from `frame+8`→`rec+5`; dispatch `(*driver)(driver,&rec)` (first vtable word). returns `ret==0` |
| `commport_dma_ring_read_frame` | `0x976e` | read one 8-B frame from the eDMA ring (ch0 vs ≠0 select distinct descriptor fields `+0xa0`/`+0xb0`, slot nibble of mode word `+0xbc`); copy 8 B to out_rec, store next-entry ptr (`src+8`) at `out_rec+8`, update producer/head idx `+0xa8`/`+0xb8` |
| `commport_can_transmit` | `0x7624` | byte-stream TX engine. ABI `{base, job, buf, len}`; reject `job==0`/`len==0`/`(buf|len)&3` (4-B align); enqueue into global + job rings, prime eDMA HW if SM idle. `COMMPORT_TX_RING = *0x78b8 = 0x20001564` |

`encode` dispatches synchronously through the driver vtable; on-wire TX bytes
ultimately flow through `commport_can_transmit` (the eDMA raw `{buf,len}` path).
`edma_tcd_build` (`0x8a7a`) builds the 4-word TCD on that submit path.
Multi-chunk RX reassembly is done by the off-image ring consumer using the
`out_rec+8` continuation pointer.

### Open items — command layer (TBC)
- The two callers of `device_dispatch_command` (`0x93fe`/`0x9404`) sit inside an
  uncarved region — the inbound-frame source channel is not yet named.
- Mapping of device-record keys / tags (`{id,0x87}`, `{id,0x91}`, `0x08c0`,
  `0x8808`) to named peer functions — no surviving name table (cf.
  application-header high-byte mapping, still UNRESOLVED).
- On-wire CAN ID field decomposition (priority/source/destination/PGN-style) and
  per-command payload schemas — entirely off-image (upper protocol layer).
- Registration sites of the four `commport_*` functions as channel/driver
  function pointers and the device-manager ops vtable — off-image.
- `xfer_waiter_reset` (`0x87fa`) and the `rtos_sem_give` path (`0x6ec0`) from
  `xfer_state_log_notify` are vendor/ambiguous, left extern.

## Open items (TBC)
- High-byte → peer-ECU mapping (no surviving name table).
- Whether multiple physical I²C segments exist or all peers share one bus (one
  shared handle observed).
- Apollo silicon revision / exact IOM instance (and reconciling with the S32K-like
  clock block).
