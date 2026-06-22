# charger — normal vs speed variant analysis

The S5 ships **two charger firmware images**, both Liteon (model `5EL00000000EB`):

| Variant | File | Version | Size |
|---|---|---|---|
| normal | `charger_liteon_normal.0.0.1.8.0.untagged.x.bin` | v0.0.1.**8**.0 | 23 340 B |
| speed (fast charge) | `charger_liteon_speed.0.0.1.2.0.untagged.x.bin`  | v0.0.1.**2**.0 | 23 076 B |

## Finding: same firmware, one charge-control parameter differs

The two images are the **same codebase**, separately compiled. Evidence:

- **Identical vector tables and reset handlers** (same SP `0x20008000`, same entry
  layout); the header at `0x134` stores each image's own size.
- A cross-binary fuzzy match aligns the functions ~1:1; **every divergent-cluster
  function diffed is logically identical** — same constants, same control flow;
  only addresses shift from recompilation (e.g. `FUN_000050c8`≡`FUN_0000501a`,
  the flash-program loop `FUN_00001e64`≡`FUN_00001e38`, the GPIO/register helpers).
- The **trailing device descriptor is byte-identical except one byte**: the word
  `0x0E1000`**`69`** (normal) vs `0x0E1000`**`09`** (speed), inside an otherwise
  shared struct (peripheral-register bases, `16 MHz`/`12 MHz` clock constants,
  the model string).
- That same value appears as an **immediate in the charge state machine**:
  `movs r0,#0x69` at `0x000031e4` (in `charge_state_dispatch` / `FUN_00003150`),
  passed to the M_CAN-Tx arming helper `FUN_00001d38`. In `speed` this is `0x09`.

So the **normal/speed difference is a single charge-control parameter**
(`0x69` = 105 → `0x09` = 9), plus version/size metadata — **not different code or
features**. `speed` is the fast-charge build; `normal` is the standard charger.
This matches the expectation that the variants differ only in how the charger is
configured for its power level.

> The exact physical meaning of `0x69`/`0x09` (a current step, a CAN message
> sub-id, or a timing divisor into `FUN_00001d38`) is not yet pinned; the value
> flows into the M_CAN transmit path from the charge dispatcher. The reconstruction
> targets the `normal` variant; `speed` is identical with this one parameter changed.
