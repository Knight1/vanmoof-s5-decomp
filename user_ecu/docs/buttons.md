# Handlebar buttons & input events — user_ecu

> Verified live in Ghidra (`user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin`,
> ARM Cortex-M4F, base `0x0`) and against `src/event.c`. All addresses base-0 hex.
> Status CONFIRMED unless marked TBC. Cross-references: `event.c` (the poller),
> `protocol.md` (the event-stream / device-manager queue and the CAN envelope),
> `led_control.md` (the button-cluster LEDs).

The user_ecu scans the **handlebar buttons** and turns each press/release into a
small record posted on the device-manager **event stream** (`mgr+0x590`), which a
comms task forwards over the comm-port/CAN bus. There are **no button strings** in
the image (logging is stripped) — the mapping below is recovered from the GPIO
scan logic, not from text.

## At a glance

| Item | Value | Evidence |
|---|---|---|
| Buttons scanned | **4** (a 4-entry scan table) | `button_scan_poll` loop cap `idx==4`, `0x3df8` |
| GPIO input window | `0x4008c000` (port stride `0x20`) | `event.c:57`, `0x3e06/0x3e0a` |
| Scan table (SRAM) | `0x20000668`, `0xc` bytes/entry | `DAT_00003ea4` |
| Per-press CAN record | 3 bytes, tag `0x00009a0b` | `input_event_post`, `0x3d14` |
| Debounce windows | **1000 ms** press, **200 ms** release/repeat | `0x3e2c` / `0x3e7e` (FreeRTOS-timer helper `0x15e0`) |
| Poller entry | `button_scan_poll` (`0x3ddc`) | installed as a dispatch callback (no direct call site) |

## Scan-table entry (`scan_entry_t`, 0xc bytes @ `0x20000668`)

Populated at init (the init data is off-image, so the **physical-button → fields
binding is TBC**); the field roles are recovered from the access pattern in
`button_scan_poll`:

| Off | Field | Role |
|---|---|---|
| `+0x0` | `event_id`  | the id byte emitted on the event stream for this button |
| `+0x1` | `port_index`| GPIO port (`<< 5` to index the `0x4008c000` window) |
| `+0x2` | `pin_offset`| byte offset of the pin's input level within the port |
| `+0x3` | `match_key` | compared against the incoming `button_id` |
| `+0x4` | `timer`     | FreeRTOS software-timer handle (debounce/repeat) |
| `+0x8` | `pressed`   | 0 = released, 1 = pressed |
| `+0x9` | `repeat`    | hold/repeat counter (saturates at `0xff`) |

## What happens on a press / release

`button_scan_poll(button_id)` (`0x3ddc`) runs per scan tick (armed via the
debounce timer; expiry callback `scan_timer_expiry` @ `0x9f87`):

1. Takes the xfer-state lock/notify (`xfer_state_lock_post`, `0x3de2`).
2. Walks the 4 entries for the one whose `timer != 0` **and** `match_key ==
   button_id`; none → return (`0x3de8..0x3df8`).
3. Samples the GPIO input level:
   `level = *(int8*)(0x4008c000 + (port_index<<5) + pin_offset)` (`0x3e00..0x3e0e`).
4. **Press edge** (`pressed==0 && level==1`, `0x3e10`): set `pressed=1`; if the
   debounce timer was still running (`timer_remaining_ticks != 0`) bump `repeat`;
   arm the **1000 ms** window; **emit press event** `input_event_post(event_id,
   0, 0)`.
5. **Release edge** (`pressed==1 && level==0`, `0x3e52`): clear `pressed`; if
   `repeat==0xff` just clear it, else arm the **200 ms** window; **emit release
   event** `input_event_post(event_id, 1, 0)`.

So each physical actuation produces **two** events — a press (`edge=0`) and a
release (`edge=1`) — both carrying the button's `event_id`. A held button cycles
the 200 ms window and increments `repeat` (auto-repeat / long-press signalling).

## The CAN data sent per button event

`input_event_post(event_id, edge, arg)` (`0x3d14`) builds a **3-byte record** and
pushes it to the device-manager event stream buffer at `mgr+0x590` via
`xStreamBufferSend` (FreeRTOS, vendor) — the same queue a comms task drains and
transmits over the comm-port/CAN bus (see `protocol.md` → *Event / status / error
records* and *CAN comm-port frame command path*).

| Field | Bytes | Value |
|---|---|---|
| stream tag (position arg) | — | **`0x00009a0b`** (`INPUT_EVENT_TAG`, `DAT_00003d44`) |
| context (ticks arg) | — | the device-manager object pointer |
| payload `[0]` | 1 | `event_id` (per-button, from the scan table) |
| payload `[1]` | 1 | `edge` — **`0` = press, `1` = release** |
| payload `[2]` | 1 | `arg` — always `0` from `button_scan_poll` |
| length | — | `3` |

> **Worked example (layout CONFIRMED; `event_id` value TBC):** pressing the button
> whose scan-table `event_id` is e.g. `0x12` posts the 3-byte payload
> `12 00 00` (tag `0x9a0b`); releasing it posts `12 01 00`. The concrete
> `event_id` per physical button lives in the off-image init table, so the
> id→button map is not recoverable from this image.

> **Gate:** `input_event_post` is a no-op until the device-manager slot
> `0x200007f0` is non-NULL (`cbz` at `0x3d1c`) — i.e. until the comms manager is
> up. The **on-wire CAN ID/DLC** for the `0x9a0b` event stream is **off-image**
> (the upper protocol layer), exactly as for the other event records in
> `protocol.md`.

## Relationship to the cluster LEDs

The handlebar button-cluster LEDs are the **two brightness channels A/B** driven
by the Q16.16 easing kernel and coupled to a companion IC over **I²C** (app header
`0x1926`); see `led_control.md`. That answers the "LEDs on I²C?" question: the LED
**brightness target is exchanged over I²C**, not driven pixel-by-pixel from this
image. The bike's head/tail lights are *separate* ECUs (`frontlight`/`rearlight`
in `manifest.txt`), distinct from these handlebar indicators.

## Open items (TBC)
- The `event_id` → physical-button mapping (off-image init data populates
  `0x20000668`).
- Which dispatch path invokes `button_scan_poll` (installed as a callback; the
  registration is off-image, like the other device-manager callbacks).
- The exact GPIO pin/port per button (`port_index`/`pin_offset` are runtime init
  values).
