# power_control — what it is and why it sits between the battery and the motor

> Reconstructed from the firmware (`docs/progress.md`, `hardware.md`) and
> cross-checked against live CAN-bus captures + the protocol map in the
> `vanmoof/canbus` tool (`CANBUS.md`, `power.go`). The two sources agree
> exactly, which is what lets this be stated with confidence rather than guessed.

## One line

`power_control` is the bike's **power-path supervisor** — the ECU that gates,
sequences and current-limits the high-voltage DC path from the **battery (DSG /
discharge side)** to the **motor controller's DC→AC PWM inverter**, while acting
as the CAN hub that commands the battery and charger and carries their telemetry.
It is a *controller*, not a power converter; the actual DC→AC conversion happens
downstream in `motor_control`.

## The power chain

```
 Battery pack (10S Panasonic/DynaPack cells)
   └ BMS: protection + DSG (discharge) MOSFET + DC-DC      ← CAN node 0xA4 "battery_primary"
        │  (high-current DC out, "Battery DSG side" cable)
        ▼
 ►► power_control  ◄◄  ← CAN node 0xA3   (NXP LPC546xx Cortex-M4F, FreeRTOS — THIS firmware)
        │  · gates / sequences / current-limits the DC rail
        │  · pre-charges the inverter, senses V/I/temperature
        │  (managed DC out, "to the Motor PWM module" cable)
        ▼
 motor_control  ← CAN node 0x8D / on-wire 0x93   ("DC AC Motor PWM conversion")
   └ 3-phase MOSFET bridge: DC → AC PWM (FOC)
        ▼
      hub motor
```

The charger (CAN node **0xA7**, Liteon) feeds the battery; `power_control` also
talks to it. The whole battery/power CAN segment uses Object-Dictionary port
`a2 = 0x82`.

## Why is this module *in between*?

A direct battery→inverter wire is unsafe and unmanageable. `power_control` is the
arbitration + protection layer that every serious EV power path needs:

1. **Safety disconnect (second-line).** A dedicated, firmware-controlled cut of
   motor power that is independent of the BMS's own DSG FET — so a fault in
   either can still open the path. The firmware commands the battery on/off
   directly (see `power_send_mode_command` below).
2. **Pre-charge / inrush protection.** The motor inverter has large DC-link
   capacitors; hard-connecting them to a charged pack draws a destructive inrush
   surge. `power_rail_enable_sequence` is exactly a soft power-up: assert an
   enable strobe → settle (delay) → wait for a hand-shake notify (≤1 s) → on
   success wait 500 ms and log success (event `0xac`); retry up to 3× then fail
   (event `0xb2`). That is a controlled contactor/pre-charge close, not a bare
   GPIO toggle.
3. **Current / power-envelope arbitration.** What the motor may draw must be
   clamped to what the battery DSG side can safely supply.
   `power_send_capabilities_frame` advertises defaults and **clamps each
   requested limit down to the advertised maximum** — power-limit negotiation.
4. **Sensing & telemetry for the whole rail.** `adc_to_temperature_lookup` reads
   an NTC thermistor (battery / MOSFET temperature, 166-entry calibration table,
   −40 °C base). `power_build_status_report` packs the live rail/flag state and
   publishes the `power_control` measurement/control OD signals.
5. **Battery & charger command + lifecycle.** `power_set_mode` drives the battery
   through power modes; on the wire that is the battery command channel.
6. **Firmware update of the battery & charger over CAN.**
   `storage_erase_program_sectors` is the page-erase/program half of the
   page-CRC-over-CAN OTA flash used to update the Panasonic/DynaPack BMS and the
   Liteon charger (0x200-byte pages, ≤0x3a sectors, 0xFF erase fill).

In short: the battery DSG cable comes *in*, the managed motor-PWM-supply cable
goes *out*, and this board decides **whether, when and how much** power flows —
plus it is the comms bridge that lets the rest of the bike (and the i.MX8 brain)
see and command the battery/charger.

## Firmware ↔ CAN-bus mapping (the proof)

Every reconstructed VanMoof function lines up with an observed CAN channel:

| Firmware function (this repo)        | CAN object / channel | What it is on the bus |
|---|---|---|
| `power_send_mode_command(op)` → CAN obj `0x1a4` = `{0xa4,0x01}` | `0x14603040` (node 0xA3→0xA4, `a1=0x01`), opcode in `frame[0]` | **Battery command**: `0x00`=OFF, `0x01`=ON, `0x05`=IdentifyCharger, `0x06`=RESET, `0x08`=ShippingMode, `0x09`=ClearFaults |
| `power_set_mode(mode)`               | (drives the above)   | The power-mode negotiation state machine (send cmd → wait ack → compare achieved → retry) |
| `power_rail_enable_sequence`         | enable strobe + notify | Controlled rail/contactor power-up (pre-charge handshake) |
| `power_send_capabilities_frame` → CAN obj `0x10a7` = `{0xa7,0x10}` | charger node `0xA7` | Charger/limit capability negotiation (clamp to max V/I) |
| `power_build_status_report` (opcode `0x2a3`) | `0x14605040` control/state, `0x14609040` measurements | `power_control` OD telemetry |
| `adc_to_temperature_lookup`          | — (local sense)      | NTC → temperature (feeds battery temp `0x14813040`: cell/chg-MOS/**dsg-MOS** temps, discharge & max current) |
| `storage_erase_program_sectors`      | `0x1490xxxx` flash ops | Battery/charger OTA page-CRC flash |
| `can_tx_task` / `can_build_event_record` / `can_request_*` / `can_send_*` | M_CAN transport | The 29-bit extended-ID Bosch M_CAN comms layer |

Bus identity from the captures: `power_control` = **PF=SA `0xA3`**, on-wire
device-encoded `0x460`, OD node `0xA3`. The neighbours it brackets are OD node
**`0xA4` = battery_primary** (the DSG side) and **`0x8D` = motor_control** (the
DC→AC PWM inverter), with **`0xA7` = charger**.

> Open hardware question (not answerable from firmware alone): whether the main
> motor current physically passes *through* this board's switch, or whether the
> board only *drives* an external contactor/pre-charge while sensing the rail.
> Both are consistent with the firmware (which commands an enable + waits for an
> ack) and with the observed cabling; the `power_rail_enable_sequence` handshake
> argues for at least pre-charge/contactor control of the path.
