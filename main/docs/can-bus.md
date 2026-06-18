# VanMoof `vm` CAN-bus protocol

The on-wire CAN protocol of the bike, recovered from the statically-linked
**`vm` library** inside the `power` service (`/S5-v1.5/OS/power` in Ghidra).
The same library backs every main-module service (`ride`, `update`, `monitor`,
…) and the bridges, so this is the **fleet-wide** wire format, not power-specific.

> Confidence: the **transport + ID encoding is adversarially verified** —
> re-derived independently from the on-disk ELF with `llvm-objdump`/`capstone`
> (separate from Ghidra), matching byte-for-byte. The per-signal address table is
> high-confidence (two independent decompilation passes agreed). Items still
> open are listed at the end.

## 1. Transport

Raw **SocketCAN** on interface **`vcan0`** (default; the physical CAN is behind
the `imx8_bridge` MCU over SPI — `spi-can-if-linux` feeds `vcan0`). A dedicated
RX `pthread` reads frames; TX is a direct `write()`.

| Property | Value |
| --- | --- |
| Socket | `socket(AF_CAN=0x1d, SOCK_RAW=3, CAN_RAW=1)` ✅ |
| Bind | `ioctl(fd, SIOCGIFINDEX=0x8933, ifr)` → `bind(fd, sockaddr_can, 0x18)` ✅ |
| Frame | classic Linux `can_frame`, **16 bytes** (TX `write(…,16)`, RX `read(…,16)`) ✅ |
| ID | **29-bit extended** (CAN_EFF); `0x80000000` OR'd at TX, bit-31 checked at RX ✅ |
| DLC | 0–8 only (`len < 9` guard) ✅ |

(✅ = independently disassembly-verified.)

## 2. Addressing — `vm_address` ↔ 29-bit CAN ID

A `vm_address` is four packed fields mapped onto the 29-bit extended ID:

```
can_id = (a0 << 21) | (a1 << 13) | (a2 << 5) | (a3 & 0x1F)        // 8 + 8 + 8 + 5 = 29 bits
on the wire: | 0x80000000 (CAN_EFF_FLAG)

a0 = (id >> 21) & 0xFF      // node / device id        bits 28..21
a1 = (id >> 13) & 0xFF      // sub-system / signal idx  bits 20..13
a2 = (id >>  5) & 0xFF      // port / message class     bits 12..5
a3 =  id        & 0x1F      // sub-id / register        bits  4..0
```

```c
struct vm_address_s { uint8_t a0, a1, a2; uint8_t a3 : 5; };
```

The in-process **`vm_frame`** is a 13-byte record `{ a0, a1, a2, a3, len, data[0..7] }`.

Worked example (verified): `0x14E23210` → `a0=0xA7, a1=0x11, a2=0x90, a3=0x10`;
`(0xA7<<21)|(0x11<<13)|(0x90<<5)|0x10 = 0x14E23210` ✓.

**TX** (`vm_can_tx`): packs the id (`lsl #21/#13/#5`, `and #0x1f`, `orr
#0x80000000`), stages the `can_frame`, `write(fd, &cf, 16)`.
**RX** (`vm_can_rx_thread`): `read(fd,&cf,16)` loop; if `len<9` and bit-31 set,
decodes id → `{a0,a1,a2,a3}` (`lsr #21/#13/#5`, `and #0x1f`) + len + ≤8 data
bytes, then hands the `vm_frame` to the dispatch.

## 3. RX dispatch & the Object Dictionary

```
vm_can_rx_thread → (backend[0]) vm_can_dispatch_enqueue → mailbox/ring
   → vm_mailbox_worker → vm_can_rx_worker_cb → vm_tp_handle_frame  (lib/src/tp/tp.c)
```

`vm_tp_handle_frame` is a **Cyphal/UAVCAN-style transport** (`tp.c`): it builds a
key `(a0 | a1<<8 | a2<<16)`, looks the address up in the **Object Dictionary
table** (a fixed-capacity, `0x50`-byte-stride array, **linear scan**), and
dispatches by a `kind` byte (0/1/2 — single vs multi-frame TP reassembly,
sub/pub direction), invoking the matched entry's callback. **The comparator
matches `(a0, a1)` only** — the node + signal index.

**Address resolution is STATIC (compile-time).** There is no runtime
`name → address` handshake and no central `{name,address}` table. Each OD signal
has its own **registration thunk** that writes a hardcoded address immediate into
the descriptor (`+0x18`=`a0`, `+0x19`=`a1`, `+0x1a`=`a2`, `+0x1b`=`a3`,
`+0x1c`=`kind`) plus a DLC, and appends it via `vm_od_register → vm_od_table_add`.
The string name (e.g. `"battery_primary_battery_voltage"`) is kept only for
dedup/de-init logging and plays no part in addressing. (A few thunks take the
`a0` node octet as a runtime parameter — "semi-dynamic" — but `a1/a2/a3` stay
baked in.)

There is **one** OD descriptor table (header at `vm_s+0x0`: `{count, cap=0x80,
cmp=0x158260, base=vm_s+0x20}`; `0x50`-byte stride). RX lookups and registration
both use it. The separate `calloc(0x2260)` at `vm_s+0x2858` is **not** a second
table — it is the **multi-frame TP reassembly pool** (100 slots × 0x58),
used only for `kind=2` transfers with DLC ≥ 9.

### `kind` byte (descriptor `+0x1c`) — TP framing selector ✅verified

| kind | meaning | RX | TX |
| --- | --- | --- | --- |
| **0** | single-frame | delivers the inline ≤8-byte payload to the handler | one frame, `write` |
| **1** | polled / request | semaphore-gated read-request | sets the **`0x10` request bit in id `a3`**, then sends |
| **2** | multi-frame (TP) | DLC<9 inline, else reassembled via the `vm_s+0x2858` pool | segmented send |

The `0x10` bit of the id's `a3` field is the **request/response direction flag**
(set = read-request that triggers a publish; clear = incoming data). All battery
telemetry signals use **kind = 2**.

## 4. The address map (recovered)

### Battery (primary / Panasonic pack) — node `a0 = 0xA4`, `a2 = 0x82`, `a3 = 0`

Nominal 29-bit ID computed as above; **RX matching is on `(a0,a1)` only**, so the
ID is the value the battery transmits (a2/a3 as observed in the descriptor).

| Signal (OD name) | a1 | DLC | nominal CAN ID | decoder | publishes |
| --- | --- | --- | --- | --- | --- |
| `…battery_charging` | 0x03 | 5 | `0x14807040` | `0x128730` | `…/charge_voltage`=u16[2:4], `…/charge_current`=u16[0:2] |
| `…battery_cell` | 0x04 | 5 | `0x14809040` | `0x12e2a0` | — (no-op stub) |
| `…battery_capacity` | 0x05 | 7 | `0x1480B040` | `0x12d150` | `…/soc`=u16[0:2]; `…/soc_app`=remap(u8[0]) |
| `…battery_warning` | 0x06 | 6 | `0x1480D040` | `0x1275a0` | ~20 alarm booleans (bit-field [0..4]) |
| `…battery_status` | 0x07 | 3 | `0x1480F040` | `0x1268d0` | ~40 status booleans; charging flag, state nibble |
| `…battery_voltage` | 0x08 | 8 | `0x14811040` | `0x1285c0` | `…/voltage`=u16[0:2] (mV) |
| `…battery_temperature` | 0x09 | 0x1a | `0x14813040` | `0x12da90` | `…/temperature`=JSON{cell 1,cell 2,chg mos,dsg mos}=u8[0..3]; `…/discharge_current`=u16[4:6]; `…/max_current`=u16[6:8]; `…/power`=(I/1000)·(V/1000) |
| `…battery_health` | 0x0A | 8 | `0x14815040` | `0x1289c0` | `…/health`=u16[4:6]; `…/cycles`=u16[6:8] |

`charging` `a1=0x03` is **confirmed** (register thunk `0x13bac0`, immediate
`0x02008203`, wrapper `0x139b70` supplies `a0=0xA4`). The battery thunk family is
a contiguous `a1 = 0x01..0x0A` run; `a1=0x01` and `a1=0x02` are additional
battery-node descriptors (cell / state-init series).

### Charger — node `a0 = 0xA7`, `a1 = 0x11`

| Frame | a2 | a3 | CAN ID | data | meaning |
| --- | --- | --- | --- | --- | --- |
| `battery_primary_battery_state_init` | 0x82 | 0x00 | `0x14E23040` | — | OD init/handshake (lives on the charger node, not `0xA4`) |
| charger clear-test #1 | 0x90 | 0x14 | `0x14E23214` | `A5 5A 00` | clears the charger's factory **test & burn-in mode** (sent first) |
| charger clear-test #2 | 0x90 | 0x10 | `0x14E23210` | `A5 5A 00` | same routine (sent second); only `a3` differs |

The two `0x14E232xx` frames are the **only hardcoded numeric CAN IDs in the
binary** — issued via `system("cansend vcan0 <id>#A55A00")` from
`power_control.cpp:0xca` (*"Clearing test & burn-in mode of charger"*), **not**
through the `vm` TX path. A raw-byte scan found zero binary occurrences of these
IDs (they exist only as ASCII command strings). `A5/5A` is the fleet's
unlock/command magic.

### Power-control OD signals (static 2-byte keys)

| OD name | a0 | a1 | a2 | a3 | kind | nominal CAN ID | register fn |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `power_state` | 0x82 | 0x01 | 0x82 | 0x01 | 2 | `0x10403041` | `0x1387d0` |
| `power_control_control` / `_state` | 0xA3 | 0x02 | 0x82 | 0x00 | 2 | `0x14605040` | `0x138b80` |
| `power_control_measurements` | 0xA3 | 0x04 | 0x82 | 0x00 | 2 | `0x14609040` | `0x138c40` |
| `power_pedal_power_switch_control_init` | 0xA2 | 0x0A | 0x82 | 0x00 | 1 (polled) | `0x14415040` | `0x139300` |
| (key `0x1a3`) | 0xA3 | 0x01 | 0x82 | 0x00 | 1 | `0x14603040` | `0x138ac0` |
| rear-carrier `_query` (`switch_control`) | 0xC3 | 0x80 | 0x82 | 0x00 | 7 | `0x1870F040` | `0x11fbe0` |

(The first workflow read `a2=0x80` for the `power_control_*` rows; the verified
re-trace shows `a2=0x82` — the whole bus uses `a2=0x82`. The runtime-`a0` thunks
`0x13b960`/`0x13bb80` resolve to the **battery** node `0xA4` (`a1` 0x01/0x04),
not power-control.)

`power_control_control` and `power_pedal_power_switch_control_init` are
registered by name in the `PowerControl` ctor but were not pinned to a specific
address word in this pass (see open items).

## 5. Function map (Ghidra `power`, base 0x100000)

| Addr | Name | Role |
| --- | --- | --- |
| `0x1582e0` | `vm_init` | inits `vm_s`, OD table at `+0x20` (cap 0x80), dispatch cb at `+0x2828`, mailbox |
| `0x158d30` | `vm_can_open` | SocketCAN socket/ioctl/bind; spawns RX thread ✅ |
| `0x158c30` | `vm_can_rx_thread` | `read(…,16)` loop, id→`vm_frame` decode, dispatch ✅ |
| `0x158b60` | `vm_can_tx` | pack id + `write(…,16)`; installed at `backend+0x20` ✅ |
| `0x1582a0` | `vm_can_dispatch_enqueue` | RX dispatch cb → mailbox |
| `0x158a30` / `0x1589b0` / `0x158900` / `0x158290` | `vm_mailbox_{enqueue,create,worker}`, `vm_can_rx_worker_cb` | the RX mailbox/ring + worker → `vm_tp_handle_frame` |
| `0x157d30` | `vm_tp_handle_frame` | real RX handler (`tp.c`): key `(a0\|a1<<8\|a2<<16)`, OD lookup, kind dispatch, TP reassembly |
| `0x157b80` | `vm_tp_publish` | TX/publish: reads entry address `+0x18` + kind `+0x1c`, emits via backend TX |
| `0x158410` / `0x158670` | `vm_od_register` / `vm_od_table_add` | append a 0x50-byte descriptor (static address) into the OD table |
| `0x158440` / `0x158860` / `0x158260` / `0x158770` / `0x157af0` | `vm_od_table_{find,scan,…_remove,…_alloc}`, `vm_od_key_cmp` | linear-scan lookup (2-byte key) + table mgmt |
| `0x1268d0` … `0x1289c0` | `battery_{status,charging,health,capacity,temperature,voltage,warning}_decode`, `soc_to_soc_app` | the per-signal payload decoders (§4) |
| `0x13bdc0` … `0x13c7f0` | `reg_thunk_{status,health,capacity,temperature,voltage,warning,cell,state_init}` | the per-signal static registration thunks |
| `0x133620` | `power_control_clear_charger_test_burnin` | the only hardcoded-CAN-ID site (the two `cansend` frames) |

## 6. Open / next

Items 1–4 of the original list (charging `a1`, the two-tables question, the
`kind` byte, the power-control addresses) are **resolved** above. Residual:

1. **Cross-reference the node scheme** against the other CAN ECU targets
   (`battery_primary_panasonic`, `motor_control`, lights, `elock`) — the same
   `vm` library + `a0` node IDs (0xA4 battery, 0xA7 charger, 0xA2 pedal, 0xA3
   power-control, 0xC3 rear-carrier) should appear there, which would confirm
   publisher/subscriber roles per signal.
2. `a1=0x02` battery-node descriptor — confirm whether it is a distinct on-bus
   signal or only the `state_init` handshake descriptor.
3. The `kind=1` polled-read path (semaphore + the `0x10` request bit) for the
   `a1=0x0A` pedal signal — trace the exact poll-vs-stream distinction.

## 7. Node map (observed)

| node `a0` | device |
| --- | --- |
| `0xA4` | primary (Panasonic) battery |
| `0xA7` (`a1=0x11`) | charger |
| `0xA3` | power-control board |
| `0xA2` | power-pedal / e-shifter switch |
| `0xC3` | rear-carrier |
| `0x82` | the main-module power node itself (`power_state`) |
