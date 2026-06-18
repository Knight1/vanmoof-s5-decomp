# ble — reconstruction progress

> Per-function tracker for the `ble` target (nRF52-class, Zephyr + Nordic
> SoftDevice Controller; image base **`0x23000`**, see `hardware.md`). Device
> addresses throughout. Full machine classification:
> `ghidra/exports/ble_classification.json`.

## Status legend
- `pending` — VanMoof function, classified, C not yet written
- `in-progress` — being decompiled/translated
- `decomp-c` — translated to `src/*.c`, compiles clean
- `byte-eq` — verified byte-identical (reloc-masked) vs OEM
- `deferred (vendor)` — Zephyr / SoftDevice Controller / nRF SDK / libc — NOT reconstructed
- `fmna-deferred` — Apple Find My; vendor, deferred (only VanMoof glue is in scope)

## Summary (after classification workflow)

A 45-agent workflow classified **2006 / 2015** functions (9 at batch edges
remain to sweep). The VanMoof application layer is **50 functions**; everything
else is vendor.

| Bucket | Count |
|---|---|
| **vanmoof** (reconstruct) | **50** |
| fmna-deferred (Apple Find My) | 118 |
| vendor-sdc (SoftDevice Controller / MPSL) | 819 |
| vendor-zephyr (kernel + BT host) | 611 |
| vendor-libc-sdk (libc / aeabi / nrfx / CC310 / arch) | 408 |
| (unclassified remainder) | 9 |
| **VanMoof reconstructed to C** | **49** (settings 2, auth 17, ble_connect 8, ble_char 5, ble_message 2, ble_msg 4, findmy_glue 10, spi_bridge 1) — `spi_bridge_consumer_thread` deferred |

> Build: `ble/` scaffold up (`Makefile`, `include/{compiler,ble}.h`); `make`
> compile-gate **clean** for all 4 TUs (`arm-none-eabi-gcc 9.2.1`, Cortex-M4F,
> `-Os -Wall -Wextra -Wpedantic -Wshadow`). These are behaviour-oriented
> translations (OEM-faithful logic + verbatim disasm comments), not yet
> byte/behaviour-verified vs OEM.

## Foundation — DONE
- [x] Target scaffolded; MCU = nRF52840/52833 + Zephyr + SoftDevice Controller.
- [x] **Image base fixed → `0x23000`** (`set_image_base` + reanalyze, saved);
      976 → 2015 functions. Memory `[[ble-image-base]]`.
- [x] Vendor-vs-VanMoof classification of all functions (workflow; export JSON).

## VanMoof frontier — 50 functions (status: `pending`)

### auth — challenge/certificate bike-security (17)
| Addr | Name | Role |
|---|---|---|
| `0x3d688` | auth_adv_extra_field_enabled | gates extra advertising field on 3 state flags |
| `0x3d6b0` | auth_handle_connection_command | dispatch open/close/lock; BLE opcodes 0xe6/0xe9-0xeb |
| `0x3d840` | auth_send_connection_state | builds + sends connection/adv state (opcode 0xe5) |
| `0x3d920` | auth_parse_certificate_challenge | decrypt/parse client cert+challenge, validate, disconnect on fail |
| `0x3dfb8` | auth_init_connection_table | zero-init per-connection table (0xb8 entries) |
| `0x3dff8` | auth_copy_pubkey_32 | copy 32-byte public key/nonce into auth buffer |
| `0x3e020` | auth_copy_bike_id | copy up to 13-byte bike-id into auth buffer |
| `0x3e11c` | auth_submit_id_event | enqueue 13-byte id auth event |
| `0x3e164` | auth_alloc_disconnect_event | alloc 0x18 auth event + handler |
| `0x3e178` | auth_alloc_connection_event | alloc 0x1c connection event + handler |
| `0x3e18c` | auth_check_connection_state | validate slot; reason 6 (challenge invalid) |
| `0x3e2d8` | auth_format_ble_address | format `%02X:..:%02X (type)` for logging |
| `0x3e350` | auth_handle_disconnect | teardown, free slot, post disconnect event |
| `0x3e3a8` | auth_handle_connect | new conn, register GATT char 0xfc0e, post event |
| `0x3e45c` | auth_init_connection_slots | init four 0x40 per-conn slots |
| `0x3e4b0` | auth_send_disconnect_reason | send auth-failure reason (cert/signature/timeout) |
| `0x3e574` | auth_alloc_event_0x14 | alloc 0x14 auth event + handler |

### ble — connect/advertise/message/transport (19)
| Addr | Name | Role |
|---|---|---|
| `0x3e588` | ble_connect_clear_adv_flag | clear adv/notify state byte when link idle |
| `0x3e59c` | ble_connect_set_ready_flag | set connect-ready byte when link-state ok |
| `0x3e5b0` | ble_send_signed_connect_payload | build COSE/CBOR-signed connect payload, send |
| `0x3e640` | ble_connect_state_machine | pairing/connect FSM on mode byte → signed send + notify |
| `0x3e6e0` | ble_advertise_bike_id_payload | copy+sign 32-byte bike_id payload, send |
| `0x3e72c` | ble_advertise_ecu_serial_payload | copy+sign 32-byte ecu_serial payload, send |
| `0x3e778` | ble_secure_session_init | init VanMoof secure-channel session (around vendor TLS) |
| `0x3e818` | ble_char_write_value_26 | length-checked char write (26B), buffer+notify |
| `0x3e860` | ble_build_const_response_13 | copy a const 13-byte response template |
| `0x3e880` | ble_send_32byte_value | copy+notify a 32-byte char value (key/id) |
| `0x3e8c8` | ble_char_write_value_13 | length-checked char write (13B), buffer+notify |
| `0x3e924` | ble_bike_id_present | predicate: 1 when a bike id is programmed (bike_id[0] != 0); writes nothing (renamed from ble_record_set_flag_byte) |
| `0x3e948` | ble_build_connect_advert_payload | compose connect advert (bike_id or ecu_serial) |
| `0x3e9a0` | ble_ftp_command_handler | `ftp_command` size/index/data/crc/status — fw/flash xfer over BLE |
| `0x3edbc` | ble_message_dispatch_by_id | parse msgpack req, dispatch 16-bit cmd id to handler table |
| `0x3f210` | ble_msg_send | frame CRC16 packet into IPC slot, transmit on comm queue |
| `0x3f2cc` | ble_msg_tx_busy_get | get transmit-in-flight/busy flag |
| `0x3f2d8` | ble_msg_tx_busy_clear | clear transmit-busy flag |
| `0x3f2e4` | ble_uicr_write_init_flag | one-time UICR provisioning: write 4 bytes to 0x10001208 via the flash controller when erased (renamed from ble_msg_send_init_error) |

### settings (2)
| Addr | Name | Role |
|---|---|---|
| `0x3c660` | settings_parse_time_pubkey | parse server response → time, `settings_vm_public_key`, `_bike_id` |
| `0x3e784` | settings_read_property_by_path | read by path key (`vm/ecu_serial`,`vm/pub_key`,`vm/bike_id`) |

### spi_bridge — comm to the main ECU (2)
| Addr | Name | Role |
|---|---|---|
| `0x3ef10` | spi_bridge_unlock | release SPI-bridge mutex (wrapper over vendor lock) |
| `0x3ef1c` | spi_bridge_consumer_thread | consumer loop: 0x55AA-framed CRC16 packets, double-buffer, flash spill — **deferred** (vendor k_poll/k_pipe dataflow unresolved) |

### findmy_glue — VanMoof glue around Apple Find My (10)
> VanMoof's own glue (routing/serialization to the comm bus). The **Apple FMNA
> core (118 fns) is deferred** — see export.
| Addr | Name | Role |
|---|---|---|
| `0x3c6f4` | findmy_handle_conn_rx | route peer RX by slot → FMNA msg build/send |
| `0x3cabc` | findmy_conn_event_handler | dispatch conn events; serialize rssi/battery |
| `0x3cdcc` | findmy_forward_peer_payload | forward peer payload chunk to FMNA slot |
| `0x3ce20` | findmy_send_status_report | build+send status report (cmd 0xc2) |
| `0x3d134` | findmy_reset_conn_slots | reset 3 FMNA conn slots (0x338 records) |
| `0x3d160` | findmy_msg_enqueue | build 0x2f8 msg record, enqueue |
| `0x3d220` | findmy_alloc_work_item | alloc 0x10 work item, stamp UUID/handler |
| `0x3d234` | findmy_match_provisioning_topic | match MQTT topic vs fmna provisioning table |
| `0x3d29c` | findmy_store_provisioning_token | store ≤16-byte provisioning token |
| `0x3d47c` | findmy_send_state_report | serialize ready/enabled/uuid/pairing/provisioned (cmd 0xe1) |

## Vendor confirmed (defer) — examples
`z_arm_pendsv` `0x41564`, fault wrapper `0x418c8`, `_isr_wrapper` `0x41670`,
SDC/MPSL ISRs `0x5f754/0x5f78c/0x5f7a6/0x5f966`, `memcpy`/`memset`
`0x3ee20`/`0x3ee62`, Zephyr boot `0x50b1c`. Full per-function buckets in
`ghidra/exports/ble_classification.json`.

## Carved so far (decomp-c)
- **settings.c** — `settings_parse_time_pubkey` (0x3c660), `settings_read_property_by_path` (0x3e784)
- **auth.c** — `auth_adv_extra_field_enabled` (0x3d688), `auth_init_connection_table` (0x3dfb8),
  `auth_copy_pubkey_32` (0x3dff8), `auth_copy_bike_id` (0x3e020), `auth_submit_id_event` (0x3e11c),
  `auth_check_connection_state` (0x3e18c), `auth_alloc_disconnect_event` (0x3e164),
  `auth_alloc_connection_event` (0x3e178), `auth_alloc_event_0x14` (0x3e574),
  `auth_format_ble_address` (0x3e2d8), `auth_init_connection_slots` (0x3e45c),
  `auth_send_disconnect_reason` (0x3e4b0);
  **auth core** — `auth_handle_connection_command` (0x3d6b0),
  `auth_send_connection_state` (0x3d840), `auth_parse_certificate_challenge`
  (0x3d920), `auth_handle_disconnect` (0x3e350), `auth_handle_connect` (0x3e3a8)
- **ble_connect.c** — `ble_connect_clear_adv_flag` (0x3e588), `ble_connect_set_ready_flag`
  (0x3e59c), `ble_send_signed_connect_payload` (0x3e5b0), `ble_connect_state_machine`
  (0x3e640), `ble_advertise_bike_id_payload` (0x3e6e0), `ble_advertise_ecu_serial_payload`
  (0x3e72c), `ble_secure_session_init` (0x3e778), `ble_build_connect_advert_payload` (0x3e948)
- **ble_char.c** — `ble_char_write_value_26` (0x3e818), `ble_build_const_response_13` (0x3e860),
  `ble_send_32byte_value` (0x3e880), `ble_char_write_value_13` (0x3e8c8),
  `ble_bike_id_present` (0x3e924)
- **ble_message.c** — `ble_ftp_command_handler` (0x3e9a0), `ble_message_dispatch_by_id` (0x3edbc)
- **ble_msg.c** — `ble_msg_send` (0x3f210), `ble_msg_tx_busy_get` (0x3f2cc),
  `ble_msg_tx_busy_clear` (0x3f2d8), `ble_uicr_write_init_flag` (0x3f2e4)
- **findmy_glue.c** — `findmy_handle_conn_rx` (0x3c6f4), `findmy_conn_event_handler`
  (0x3cabc), `findmy_forward_peer_payload` (0x3cdcc), `findmy_send_status_report`
  (0x3ce20), `findmy_reset_conn_slots` (0x3d134), `findmy_msg_enqueue` (0x3d160),
  `findmy_alloc_work_item` (0x3d220), `findmy_match_provisioning_topic` (0x3d234),
  `findmy_store_provisioning_token` (0x3d29c), `findmy_send_state_report` (0x3d47c)
- **spi_bridge.c** — `spi_bridge_unlock` (0x3ef10)
  [`spi_bridge_consumer_thread` (0x3ef1c) deferred]

## Next steps
1. [x] Rename the 50 VanMoof functions in Ghidra (synced with export; saved).
2. [x] Build setup for `ble` (Makefile, `include/`) — Cortex-M4F compile gate, clean.
3. [x] Carve settings + auth helpers (18 fns, above).
4. [x] Carve the **auth core**: `auth_handle_connect`, `auth_handle_disconnect`,
   `auth_handle_connection_command`, `auth_send_connection_state`,
   `auth_parse_certificate_challenge` (signature/CBOR cert validation; vendor
   crypto `cbor_verify_signature`/CC310 left extern).
5. [x] Carve **ble connect/char/message** (15 fns): the connect FSM + signed
   advert chain, the GATT char read/write helpers, `ble_ftp_command_handler`
   (firmware/flash transfer), `ble_message_dispatch_by_id`, `ble_msg_send`, and
   `ble_uicr_write_init_flag`. Verified `0x622b8` = the flash controller
   (shared), `0x642b8` = "n", and the `FTP_BLOB_TABLE` layout against the image.
   Renamed two misnamed functions to match verified behaviour (see below).
6. [x] Carve **findmy_glue** (10) and **spi_bridge** (`spi_bridge_unlock`).
   `spi_bridge_consumer_thread` (0x3ef1c) is **deferred** (see below).
7. Finish **spi_bridge_consumer_thread**: pin the Zephyr `k_poll_event` /
   `k_pipe` stack-struct layout and the inbound-length dataflow, then reconstruct.
8. Sweep the 9 unclassified remainder functions; carve the newly-discovered
   helpers (`findmy_build_message` 0x58b12, `ble_msg_publish_clear_59bac` 0x59bac,
   `ble_announce_command_id` 0x58aa8).

### Notes from batch 4
- **Deferred:** `spi_bridge_consumer_thread` (0x3ef1c). The framing/CRC/window
  logic is VanMoof and documented, but the body is dominated by vendor Zephyr
  `k_poll`/`k_pipe`/ring plumbing; the inbound received-length dataflow (the
  count that drives the deframe loop) is not yet resolved, so it was not shipped
  as a speculative reconstruction.
- **ABI fix:** `findmy_match_provisioning_topic` real signature is
  `void *(const char *topic, uint32_t len)` (was a placeholder `void *(void)`);
  `ble.h` updated. Only used as a code pointer, so the existing reference is
  unaffected.
- **Classification:** the JSON-writer helpers `ble_json_key_585a2` (0x585a2) and
  `ble_json_add_field_5feae` (0x5feae) are vendor (same family as the existing
  `ble_json_*`), not VanMoof. The CRC at `0x58d7c` (seeded entry) is distinct
  from the seed-0 wrapper `ble_crc16_58d72`. `0x4f318` (msgq-put) is one shared
  vendor primitive (`comm_msgq_put_4f318`).
- **Newly-discovered VanMoof helper:** `findmy_build_message` (0x58b12, FMNA
  framing) — forward-declared, carve later.

### Notes from batch 3
- **Renames** (verified from disassembly; Ghidra + export synced):
  `ble_record_set_flag_byte` → **`ble_bike_id_present`** (read-only predicate),
  `ble_msg_send_init_error` → **`ble_uicr_write_init_flag`** (writes a UICR word
  via the flash controller; not a message send).
- **Newly-discovered VanMoof helpers** (not in the original 50; declared as
  forward decls, carve later): `ble_msg_publish_clear_59bac` (0x59bac, publish
  with NULL payload), `ble_announce_command_id` (0x58aa8, JSON announce builder).
- **Name caveat (kept):** `ble_advertise_bike_id_payload` / `_ecu_serial_payload`
  load URL prefixes crossed vs their names; the caller compensates so each
  emitted URL matches its buffer content. Net behaviour correct.
- **Flagged for byte-verification:** `ble_ftp_command_handler` `cmd 3` reproduces
  an OEM quirk (reads the "crc" cursor while uninitialised when "size" is
  absent); behaviour-oriented, matches Ghidra's decompile.
