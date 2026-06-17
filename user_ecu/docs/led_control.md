# LED control — user_ecu LED rings

> Verified live in Ghidra (`user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin`,
> ARM Cortex-M4F, base `0x0`). All addresses base-0 hex. Status CONFIRMED unless
> marked TBC. Cross-references: `control.c` / `control.h` (the easing kernel),
> `protocol.md` (the I²C framing, the device-dispatch frame, the comm-port/CAN
> envelope), `hardware.md` (the FTM bring-up).

The S5 carries **two LED rings, channel A and channel B** (`g_ctrl_struct_A` @
`0x20001ce4`, `g_ctrl_struct_B` @ `0x20000860`). They are **not** driven by a
"set brightness" command. Each loop iteration the control task reads physical
sensors, scales them to two counts, exchanges them with a companion IC over I²C
(app header `0x1926`), receives two brightness **targets**, and feeds each
target through a per-channel **signed Q16.16 easing kernel**
(`ledEasing_ControlUpdate` @ `0xc64`). The two eased, quantized levels are then
published outward as a CAN telemetry record (device key `0x04c0`). The loop is
free-running with a ~2.5 s cadence and never returns. The bike's LED ring is
therefore a **sensor-driven closed loop** in this image, observed on CAN but not
commanded on CAN (see *What you cannot do from CAN*).

## Control chain (end to end)

The pipeline, with every stage's function + OEM address. All in
`vPortStartFirstTask_ControlTask` (`0x370`, the control-task body) except where
noted; the call sequence per loop iteration is verified at `0x550..0x76e`.

1. **Physical sensors** —
   - `i2c_reg_read_153(0xd, buf, 3)` (`0x1a70`, called `0x5a4`): 3-byte raw
     register, masked to 18 bits (`raw = b0 + b1·0x100 + (b2 & 3)·0x10000`,
     `0x5b4..0x5c8`).
   - `sensor_read_sht_temp_humidity(0xd, …)` (`0x1e1c`, called `0x62c`).
   - `sensor_read_sht_temp_humidity(0x10, …)` (`0x1e1c`, called `0x664`).
2. **Scale + fold into the source floats** — reg-`0xd` raw is converted to float
   (`base@0x20000804 + raw·0.45/3.0 · scale@0x20000004`, `0x5d0..0x624`); the two
   SHT reads fold into the easing input floats:
   - `g_sensor_src_1` @ `0x2000000c` ← `@0xd` temp/hum (`vstr` @ `0x660`).
   - `g_sensor_src_0` @ `0x20000008` ← `@0x10` temp/hum (`vstr` @ `0x698`).
3. **Scale sensors → counts** — `controlTask_CmdHandler(0x1c)` (`0x1ee0`, called
   `0x69c`) and `(0x1d)` (called `0x6b6`) compute, from the source floats:
   - `count_a = (uint16)((g_sensor_src_0 · 65535.0) / 100.0)`
   - `count_b = (uint16)(((g_sensor_src_1 + 45.0) · 65535.0) / 175.0)`
   Pool `@0x1fd8`: `65535.0f=0x477fff00`, `100.0f=0x42c80000`, `45.0f=0x42340000`,
   `175.0f=0x432f0000`. (CONFIRMED `0x1f02..0x1f50`.)
4. **Companion I²C exchange** — header tag **`0x1926`** built at `frame[0..1] =
   {0x26,0x19}`; `count_a` then `count_b` appended via `frame_append_word_crc`
   (`0x899c`, per-word CRC-8 poly `0x31`); TX `i2c_tx_frame` (`0x7408`),
   `busyWait_Ticks(0x32)`, RX 4 halfwords `i2c_rx_frame_verify(4)` (`0x73a0`).
5. **Reply → targets** — the first two reply halfwords are byte-swapped (`rev16`,
   `0x1f84/0x1f86`): `out_a = rev16(h[0])`, `out_b = rev16(h[1])`. These are the
   easing **targets**. (`h[2]/h[3]` are CRC-checked but unused.)
6. **Easing** — tick counter `0x2000080c` is bumped once (`0x1f94`), then
   `ledEasing_ControlUpdate(&g_ctrl_struct_A, out_a, &res_a)` (`0x1f9e`) and
   `(&g_ctrl_struct_B, out_b, &res_b)` (`0x1fa8`). `get_xrefs_to 0xc64` =
   exactly `{0x1f9e, 0x1fa8}` — the kernel has no other call site. (CONFIRMED.)
7. **Quantized level** — the kernel writes a round-to-nearest **signed
   whole-unit** level to `*out` (Q16.16 `>>16` with a negative-symmetric fixup,
   `0xc7c..0xc92`). cmd `0x1c` returns channel A's level, cmd `0x1d` returns
   channel B's.
8. **PWM duty (FTM)** — **TBC / off-image.** `main_SystemInit` programs the FTM
   period/prescale/dead-time at base **`0x4009d000`** (`0x49a6..0x4b02`) but
   **never writes a channel-value (CnV) duty register**, and no reachable
   function writes `0x4009d000 + duty_offset`. The per-tick duty write, if it
   exists, lives in an off-image task body (see *Reference*). In this image the
   eased level leaves only via the CAN telemetry record (step below), not via a
   local PWM write.
9. **CAN telemetry** — the two eased levels plus the sensor fields are packed
   into the device record keyed `0x04c0` and transmitted via `device_apply`
   (`0x8656`, called `0x736`). See *Observing the LED over CAN*.

> The easing **target** is the byte-swapped `0x1926` reply, whose inputs are the
> sensor floats. The reply originates from an **off-image companion IC** over
> I²C — the actual brightness numbers are not computed in this image.

## The easing kernel

`ledEasing_ControlUpdate(state*, int target, int *out)` (`0xc64`,
`control.c:132`) advances one channel by one tick. A *target* is a scaled sensor
count (1..64999 tracked; gate `(uint32)(target-1) < 64999` ≡ `≤ 0xfde6`,
`0xc9a..0xca2`).
The state struct is `0xa0` bytes (`control.h:33`). Stages, in order:

| Stage | What it does | OEM |
|---|---|---|
| Startup gate | While `tick(+0x2c) ≤ 0x2d0000`, only `tick += 0x10000` and skip straight to the quantizer. First **45** ticks (`0x2d0000/0x10000`) hold the output. | `0xc6c..0xc7a` |
| Command tracker | `cmd_step(+0x08)` chases `target`: if below, advance up to `+0x7fff`/tick (clamped to not overshoot); else `+1`/tick. `delta(+0x30) = (next-cur)<<16`. (Note: only `delta` carries the step; `cmd_step` itself is not written back in this path.) | `0xca4..0xcb2` |
| Variant gate | If `variant(+0x00)==0` **or** `out_init(+0x38)≠0`: run the exp/sigmoid shaping. Else (a not-yet-started variant-1 channel) hold `out_value(+0x34)=level_base(+0x04)`. | `0xcb4..0xcc0` |
| Exp/sigmoid shaping | `norm = (delta-off_78)/denom`; `v = q16_mul(norm, gain_scale)`; exponent `e = off_7c·(v-off_80)`; clamp `e<-50.0→v=500.0`, `e>50.0→v=0`; else a logistic shaping using `q16_exp` and a per-variant bias term. | `0xcc2..0xd3c`, `0xe1e..0xe5e` |
| Triple-EMA blend | Seed `smooth_a/b/c(+0x94/+0x98/+0x9c)` on first run; `smooth_a/b = lerp(…, off_88/off_8c)`; adaptive blend factor from `\|smooth_a-smooth_b\|`; `out_value` floored at `0x8000` (0.5 unit). | `0xd3e..0xde4` |
| Momentum integrator | Runs only while `delta>0`: settles a position accumulator (`off_3c/off_40`), advances two sigmoid-input ramps (`off_60/off_64`), evaluates `q16_sigmoid` six times, updates a radial/length state (`off_44`) via `q16_sqrt` geometry, commits `off_74=off_44`, `off_78=off_40+off_3c` for next tick. | `0xde6..0x102c` |
| Quantize | `q = out_value + 0x8000`; if negative use `0xffff8000 - out_value`; arithmetic `>>16`; restore sign; `*out = q`. Round-to-nearest signed whole unit. | `0xc7c..0xc92` |

**Channel A vs B (CONFIRMED, corrects the L/R-only framing):** the init
`vPortStartFirstTask_ControlTask` calls `ledParams_Init(0x20001ce4, variant=0)`
(`0x488..0x48c`) and `ledParams_Init(0x20000860, variant=1)` (`0x492..0x494`).

| | Channel A (`0x20001ce4`) | Channel B (`0x20000860`) |
|---|---|---|
| variant `+0x00` | 0 | 1 |
| level_base `+0x04` | `0x640000` (100.0) | `0x10000` (1.0) |
| cmd_step `+0x08` | `0x4e20` (20000) | `0x2710` (10000) |
| normalize denom | `-0xdc0000 - off_74` | fixed `0x7d00000` |
| off_7c / off_80 | `0xfffffe56` / `0xd50000` | `-0x296` / `0x02660000` |
| off_84 (bias branch) | `0x640000` (≠`0x10000`) | `0x10000` (selects bias path) |
| easing gate | always eases (variant 0) | holds at 1.0 until `out_init` set |

Shared by both: `gain_scale=0xe60000`, `off_28/off_44/off_74=0x320000`,
`off_48=q16_div(0x2469,0xc0012)`, `off_4c=q16_div(0x48d,0xc0012)`,
`off_54=0x68d`, `off_88=0xc31`, `off_8c=0x83`; all counters/flags zeroed.
cmd `0x1c` returns channel A's level, cmd `0x1d` channel B's. Which ring is
physically left/right is **TBC** (no surviving symbol).

> **q16_exp/q16_sigmoid table caveat (NUMERICS UNVERIFIED).** `q16_exp` (`0xb04`)
> is structurally natural exp — clamps `+10.38`/`−11.78` match
> (`@0xb50`: hi `0x000a65ae`, lo `0xfff4376e`) — but the factor tables in **this
> dump** are not valid exp multipliers: pos `tab[0] @0xa4f0 = 0x10002800`, whereas
> `e^1.0` should be `0x0002b7e1`. The control flow, the 45-tick gate, the
> 1..64999 window, the `≤0x7fff`/tick slew, the variant presets, the A=variant0/
> B=variant1 assignment, and the quantizer are CONFIRMED; the **exact shaped
> brightness curve is NOT reproducible** until the real `0xa4e0/0xa4f0` tables
> are recovered (consistent with this image's other off-image/decoy anomalies).

`q16_sigmoid(self,x)` (`0x847c`) = `1/(1+exp(gain·(x-x0)))` with `gain@+0x6c`,
`x0@+0x70` rewritten by the kernel before each of its 6 calls (gains `0x28f`,
`0x170a`). Its output inherits the `q16_exp` caveat.

## Observing the LED over CAN (output)

Each loop iteration the task publishes a single device record keyed **`0x04c0`**
(`key = {0xc0,0x04,0x00}`, byte stores `0x6d4..0x6dc`). `registry_lookup_value`
(`0x830a`, called `0x6e4`) returns the `0x2c`-byte record; under its access
semaphore (`xQueueSemaphoreTake(rec[1], 100)`, `0x6f6`) **10 payload bytes** are
copied into `rec->data` (`str/str/strh` @ `0x708..0x716`); a re-lookup +
`device_apply(mgr, rec)` (`0x8656`, called `0x736`) transmits it, then the
semaphore is released and the comm-port queue is poked (`rtos_sem_give 0x6ec0`,
`0x752`).

**Payload (10 bytes, little-endian within each word, CONFIRMED):**

| Off | Field | Source | strh |
|---|---|---|---|
| `+0x00..01` | reg-`0xd` whole value `(int16)` | `i2c_reg_read_153(0xd)` scaled | `0x624` |
| `+0x02..03` | temp/hum `@0xd` `(u16)` | `sensor_read_sht_temp_humidity(0xd)` | `0x78c` |
| `+0x04..05` | temp/hum `@0x10` `(u16)` | `sensor_read_sht_temp_humidity(0x10)` | `0x7ea` |
| `+0x06..07` | **LED-A level** | `controlTask_CmdHandler(0x1c)` (channel A easing out) | `0x7f2` |
| `+0x08..09` | **LED-B level** | `controlTask_CmdHandler(0x1d)` (channel B easing out) | `0x7fa` |

**Worked decode (CONFIRMED layout; example values illustrative):** an on-MCU
`rec->data` of

```
3C 00 | 9A 0B | C4 0A | 64 00 | 01 00
```

decodes to: reg-`0xd` whole = `0x003C` = 60; temp/hum`@0xd` = `0x0B9A`; temp/hum
`@0x10` = `0x0AC4`; **LED-A level = `0x0064` = 100**; **LED-B level = `0x0001`
= 1** (note 100 / 1 are exactly the two `level_base` presets, i.e. the channels
sitting at their startup hold values). `device_apply` then stamps the 3-byte key
`{0xc0,0x04,0x00}` from `rec+0x0c` and sends one frame (TYPE 0) over the manager
channel (`mgr+0x594`, send method `mgr+0x5a4`); see `protocol.md` *Outbound
command-frame build* for the 13-byte `device_apply` frame
(`key[0..2] | seq/flags | len | payload[8]`).

> **The on-wire CAN ID/DLC for key `0x04c0` is OFF-IMAGE.** Only the 10-byte
> on-MCU payload and the 3-byte key are in this image; the CAN-ID decomposition
> (priority/source/PGN-style) and the channel send-method registration are in the
> off-image upper protocol layer (`protocol.md` *CAN comm-port frame command
> path*). To observe the LED levels on a real bus you must first map this key to
> its CAN frame externally.

## Commanding the control subsystem over CAN (input)

Inbound CAN commands enter through `device_dispatch_command` (`0x41a4`). The
frame format (CONFIRMED `0x41aa..`, full detail in `protocol.md` *Inbound command
message + dispatch*):

| Off | Field |
|---|---|
| `+0` `+1` `+2` | `key[0..2]` → `key24 = msg[0] \| msg[1]<<8 \| msg[2]<<16` |
| `+3` | flags (bit `0x10` = streaming/ack qualifier; bits `0..2` = fragment sub-index) |
| `+4` | len |
| `+5..` | payload |

`registry_lookup_value(mgr, key24)` selects the `0x2c`-byte record; dispatch
branches on TYPE (`rec+0x10`) / MODE (`rec+0x0f`) and calls the handler
`rec+0x14` as `handler(ctx=rec+0x18, msg, buf, len)`. The two callers of
`0x41a4` (`0x93fe/0x9404`) are tail thunks with **no in-image xref** — the
inbound source channel and all handler registrations are off-image.

**Worked example — the verified status-read path:** the only inbound
command that reaches the control task is the `0x09c0` status record, whose
handler is `device_fetch_cache_status9c0` (`0x410c`, no in-image caller →
off-image registered). Frame:

```
C0 09 00 | 10 | 01 | 01
└ key24=0x0009C0 ┘  │   │   └ payload[0] == 1  (the cmd[0]==1 gate, 0x4110)
                    │   └ len = 1
                    └ flags  (TBC — exact bit-0x10 requirement is off-image)
```

> **Flags byte TBC.** Whether bit `0x10` must be SET vs CLEAR for the `0x09c0`
> record is governed by that record's MODE byte (`rec+0x0f`), and the `0x09c0`
> record/handler is **registered off-image** (`device_fetch_cache_status9c0`
> @`0x410c` has no in-image xref — CONFIRMED). `device_dispatch_command`
> @`0x41a4` shows MODE 0 requires bit `0x10` CLEAR (`& 0x10 != 0 → return -1`)
> while MODE 1 requires it SET (`<<0x1b` sign test); the value shown above
> assumes MODE 1, which is not determinable from this image. The
> in-image-CONFIRMED parts of this example are: the `payload[0]==1` gate
> (`0x4110/0x4114`), the `0x3a` call (`0x411c`), the key `{0xc0,0x09,0x00}`,
> and the `device_apply` re-emit (`0x4192`).

On dispatch, `device_fetch_cache_status9c0` checks `payload[0]==1`
(`ldrb; cmp #1`, `0x4110..0x4114`), calls `controlTask_CmdHandler(0x3a)`
(`0x411c`) — a **status read** (app tag `0xe28` via `i2c_read_status_e28`
`0x8964`) — caches the u16 status into the `{0xc0,0x09,0x00}` record's data
(`strh`, `0x4182`), and re-emits via `device_apply` (`0x4192`). This **reads**
a status word; it does **not** set any LED target.

**The actual target-injection point is the companion I²C `0x1926` exchange**, not
a CAN command. The TX request (`controlTask_CmdHandler` `@sp+0x8`, CONFIRMED):

```
26 19 | <count_a hi> <count_a lo> <crc8> | <count_b hi> <count_b lo> <crc8>
└ 0x1926 ┘└──── per-word CRC-8 poly 0x31 ────┘└──── per-word CRC-8 ────┘   (8 bytes, opcode 0x59)
count_a = (uint16)((g_sensor_src_0 · 65535)/100)
count_b = (uint16)(((g_sensor_src_1 + 45) · 65535)/175)
```

Worked: `g_sensor_src_0 = 50.0` → `count_a = floor(50·65535/100) = 32767 =
0x7FFF`; `g_sensor_src_1 = 30.0` → `count_b = floor((30+45)·65535/175) = 28086 =
0x6DB6`. Frame = `26 19 | 7F FF <crc> | 6D B6 <crc>`. The reply
(`i2c_rx_frame_verify(4)`, 4 halfwords, per-word CRC verified) supplies the
targets:

```
out_a = rev16(reply.h[0])  → ledEasing_ControlUpdate(g_ctrl_struct_A, out_a, &res_a)
out_b = rev16(reply.h[1])  → ledEasing_ControlUpdate(g_ctrl_struct_B, out_b, &res_b)
reply.h[2], reply.h[3] : received + CRC-checked, not consumed
```

> Injecting a target requires answering the `0x1926` I²C request **as the
> companion peer** with chosen `out_a`/`out_b` halfwords (big-endian on the wire,
> byte-swapped on receipt). There is no CAN frame in this image that writes
> `out_a`/`out_b`, `g_sensor_src_*`, or the channel structs directly.

## What you cannot do from CAN (honest limits)

- **There is NO direct LED-brightness/level setpoint command in this image.** The
  easing kernel (`0xc64`) is invoked **only** from `controlTask_CmdHandler`
  cmd `0x1c/0x1d`, and those run **only** inside the sensor loop
  (`vPortStartFirstTask_ControlTask`). Their target is the byte-swapped `0x1926`
  reply, whose inputs are the sensor floats.
- **The LED is sensor-driven.** `g_sensor_src_0` (`0x20000008`) and
  `g_sensor_src_1` (`0x2000000c`) are written **only** by the sensor loop (`0x698`
  / `0x660`) and read **only** by `controlTask_CmdHandler` (`0x1f06` / `0x1f18`).
  No inbound-command path writes them. The channel structs `0x20001ce4` /
  `0x20000860` have no direct data xrefs (reached only via the literal pool).
- **External override needs an off-image handler.** The only inbound→control
  hook (`device_fetch_cache_status9c0`, cmd `0x3a`) is a status **read**. Driving
  brightness would require a *different* device-record handler (`rec+0x14`) that
  calls `controlTask_CmdHandler(0x1c/0x1d)` or writes `g_sensor_src_*` / the
  channel structs — no such handler is present or registered in this image.
- **The on-wire CAN ID/DLC → command mapping is off-image.** Device-record
  handler-pointer registration and the CAN-ID decomposition live in the off-image
  upper protocol layer; `device_dispatch_command`'s callers are unreferenced
  tail thunks (`protocol.md`).
- **The exact brightness curve is unverified.** `q16_exp`/`q16_sigmoid` numerics
  depend on anomalous factor tables (`0xa4e0/0xa4f0`); shaped output values are
  not reproducible from this dump (see *easing kernel* caveat).
- **No PWM duty write in-image.** The FTM at `0x4009d000` is brought up but never
  receives a per-tick CnV duty; if a duty write exists it is in an off-image task
  body. (Also: the "duty bytes at `DAT_00004c30+0x32b/0x32c=0x40`" in older notes
  are actually **NVIC_IPR** priority writes — `0x4c30 = 0xe000e100`, so the
  stores land at `0xe000e42b/0xe000e42c` = IRQ 43/44 priority, **not** FTM duty.)

## Reference — addresses & globals

| Symbol | Addr | Role |
|---|---|---|
| `vPortStartFirstTask_ControlTask` | `0x370` | control-task body; sensor→easing→telemetry loop (~2.5 s) |
| `controlTask_CmdHandler` | `0x1ee0` | cmd `0x1c/0x1d` (easing) / `0x3a` (status read) |
| `ledEasing_ControlUpdate` | `0xc64` | per-tick Q16.16 easing kernel (body `0xc64..0x1032`) |
| `ledParams_Init` | `0xb60` | per-channel preset init (variant 1 vs other) |
| `q16_exp` | `0xb04` | natural exp (factor tables `0xa4e0/0xa4f0` anomalous → numerics TBC) |
| `q16_sigmoid` | `0x847c` | logistic `1/(1+exp(gain·(x-x0)))`, gain `+0x6c`/x0 `+0x70` |
| `i2c_tx_frame` / `i2c_rx_frame_verify` | `0x7408` / `0x73a0` | `0x1926` frame TX / RX |
| `i2c_read_status_e28` | `0x8964` | `0x3a` status read (tag `0xe28`) |
| `frame_append_word_crc` | `0x899c` | per-word CRC-8 poly `0x31` append |
| `registry_lookup_value` | `0x830a` | 3-byte key → `0x2c`-byte device record |
| `device_apply` | `0x8656` | single-frame outbound builder (telemetry TX) |
| `device_dispatch_command` | `0x41a4` | inbound CAN command dispatcher (callers off-image) |
| `device_fetch_cache_status9c0` | `0x410c` | inbound→control hook: cmd `0x3a` status read (off-image registered) |
| `g_ctrl_struct_A` | `0x20001ce4` | channel A state (variant 0, level_base 100.0) |
| `g_ctrl_struct_B` | `0x20000860` | channel B state (variant 1, level_base 1.0) |
| `g_sensor_src_0` | `0x20000008` | easing source float (from `@0x10` temp/hum) |
| `g_sensor_src_1` | `0x2000000c` | easing source float (from `@0xd` temp/hum) |
| `g_ctrl_tick_counter` | `0x2000080c` | bumped once per easing pass |
| FTM base | `0x4009d000` | LED-PWM timer (period/prescale/dead-time only; no in-image duty write) |
| Telemetry key | `0x04c0` | `{0xc0,0x04,0x00}` outbound LED+sensor record (10 B payload) |
| Status key | `0x09c0` | `{0xc0,0x09,0x00}` inbound status-read record |
| App tag (setpoint req) | `0x1926` | companion-IC target exchange (8 B TX / 4 hw RX) |
| App tag (status) | `0xe28` | status word read (cmd `0x3a`) |