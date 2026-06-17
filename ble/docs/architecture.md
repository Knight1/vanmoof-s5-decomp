# ble — architecture & module map

> What the `ble` firmware is and how the VanMoof application layer sits on top
> of the vendor stacks. Device/link addresses (image base `0x23000`, see
> `hardware.md`). Status from the string/xref recon after the base fix;
> per-function reconstruction is tracked in `progress.md`.

## What it is

The `ble` SoC is the bike's **Bluetooth Low Energy gateway to the phone app**.
It runs **Zephyr** + Nordic **SoftDevice Controller**, and the VanMoof code on
top implements:

- **Authenticated connections** — a challenge / X.509-certificate scheme that
  gates the bike (public key, bike id, server signature, certificate
  expiry/blacklist, challenge-response, auth timeout).
- **The phone pairing/connect path** — `vmf://connect?bike_id=…` and
  `vmf://connect?main_ecu_serial=…` URIs (the QR / NFC tap-to-connect), the
  app id `nl.samsonit.vanmoofapp`.
- **A BLE message protocol** — `ble_message_fmt`, a `ble_tx_thread`, internal
  event routing between modules.
- **Settings** — persisted `vm/pub_key`, `vm/bike_id`, `vm/ecu_serial`
  (`settings_vm_public_key` / `_bike_id` / `_serial_number`) in the Zephyr
  settings/NVS store (`FLASH_0`).
- **Device commands** — `factory_reset`, `reboot` (`Performing %4s-reboot`),
  `version_info_req`, `reset_reason_req`, `modem_system_time`, `app_update`.
- **Apple Find My (FMNA)** integration — `fmna_*` (pair / sound /
  motion_detection / serial_number / state), owner / non-owner / config event
  streams, unwanted-tracking, the FMNA crypto labels (`PairingSession`,
  `ServerSharedSecret`, `SerialNumberProtection`, `diversify`,
  `intermediate`). **Borderline vendor — flag, do not blindly reconstruct**
  (Apple MFi Find My reference design; only VanMoof glue is in scope).

## Vendor vs VanMoof (decomp scope)

Per project rules, **reconstruct only VanMoof-authored code**; the rest is
identified and deferred as vendor.

**Vendor (defer):**
- **Zephyr RTOS** — kernel (`z_arm_pendsv`, fault handlers, scheduler), the
  Bluetooth **host** (`subsys/bluetooth/host/hci_core.c`, `conn.c`, …),
  settings/NVS, logging.
- **Nordic SoftDevice Controller + MPSL** — link layer, `BT CTLR ECDH`, the
  radio/timer ISRs (`0x5f754…0x5f966`).
- **nRF / nRF Connect SDK HAL + CC310** crypto, CMSIS, libgcc/`__aeabi_*`,
  compiler `memcpy`/`memset` (`FUN_0003ee20`/`FUN_0003ee62` are memcpy/memset).

**VanMoof app (reconstruct)** — the auth / conn / ble_message / settings /
command / fmna-glue layer above. First confirmed anchors (device addrs):

| Function | Role (from xref'd strings) |
|---|---|
| `FUN_0003e640`, `FUN_0003e72c` | build the `vmf://connect?bike_id=` / `…main_ecu_serial=` URIs |
| `FUN_0003e9a0` | string match helper (`Comparing %s with %s (%d characters)`) |
| table @ `0x3ca10` | command/reboot dispatch (`factory reset`, `Performing %4s-reboot`) |
| `FUN_00050b1c` | (vendor) Zephyr boot banner path |

> Many VanMoof identifiers (`auth_module`, `conn_module`, `ble_tx_thread`,
> `fmna_*`) are **Zephyr log-module / thread names** referenced from *data*
> structs (not direct code `LDR`), so they don't show code xrefs yet; they will
> anchor modules once the log_const / thread tables are typed.

## Reconstruction status

Foundation only so far: target scaffolded, MCU identified, **image base fixed
to `0x23000`** (the hard part — nothing resolved before it), program
reanalysed (**2015** functions). VanMoof frontier enumeration + per-function C
translation is the next phase. See `progress.md`.
