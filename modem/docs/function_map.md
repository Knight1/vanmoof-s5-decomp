# modem — VanMoof application function map

37 app functions located among the 1,334 Zephyr/nRF functions and reconstructed to C in `src/`.

| Addr | Name | Subsystem | File | Conf | Role |
|---|---|---|---|---|---|
| `0x0001378` | `wdt_heartbeat_cb` | ftp_hb_main | app_main.c | medium | WDT interrupt service routine / timeout callback installed by wdt_heartbeat_cb_setup. Fire |
| `0x00012528` | `spi_process_thread` | spi_bridge | spi_bridge.c | medium | Zephyr k_work_q thread body: waits for work items enqueued by the consumer thread (or othe |
| `0x000132b8` | `lte_link_event_handler` | ppp_lte | ppp_lte.c | medium | MQTT subscription handler for LTE modem events (CEREG / AT+CGEREP=1 notifications). On reg |
| `0x000133a8` | `ppp_state_dispatch` | ppp_lte | ppp_lte.c | high | MQTT message handler for PPP control: reads a 'start' flag from the payload; if set calls  |
| `0x00013408` | `reboot_handler` | ftp_hb_main | app_main.c | medium | MQTT reboot topic handler (topic code 0x10c1). Receives a CBOR-encoded reboot request cont |
| `0x0001385c` | `modem_lte_config_handler` | ppp_lte | ppp_lte.c | medium | MQTT handler for the 'modem_lte_config' subscription topic. Issues AT+CGCONTRDP=0 to query |
| `0x000139b8` | `modem_nordic_update_config_handler` | ppp_lte | ppp_lte.c | medium | MQTT handler for the 'modem_nordic_update_config' subscription topic. Parses a CBOR payloa |
| `0x00013b34` | `modem_signal_report` | ppp_lte | ppp_lte.c | medium | Publishes current LTE modem signal quality (RSRP, RSRQ, SNR, band, PLMN, firmware version) |
| `0x00013f3c` | `modem_app_init` | ppp_lte | ppp_lte.c | medium | Top-level application init: initialises nRF modem lib, sets up TLS credentials, registers  |
| `0x0013F3C` | `modem_main` | ftp_hb_main | app_main.c | high | Main LTE connectivity and MQTT setup loop. Single caller: NMI_Handler. Initialises modem,  |
| `0x000145d4` | `ppp_data_forward_loop` | ppp_lte | ppp_lte.c | medium | Data-forwarding thread body: polls the modem socket fd, reads available data with recv, an |
| `0x0014898` | `wdt_timer_init` | ftp_hb_main | app_main.c | medium | WDT and MQTT timer initialisation called from modem_main. Sets up the WDT device struct fi |
| `0x0001493c` | `ppp_carrier_on` | ppp_lte | ppp_lte.c | medium | PPP start: marks the subsystem active, drives PPP carrier-on on the Zephyr network interfa |
| `0x000149c4` | `ppp_sockets_close` | ppp_lte | ppp_lte.c | medium | Tears down the PPP socket pair: closes the modem PPP socket and the Zephyr PPP socket, dri |
| `0x00014b48` | `ppp_socket_setup` | ppp_lte | ppp_lte.c | medium | Creates and configures the PPP modem socket and the Zephyr PPP network socket, applies SO_ |
| `0x00158BC` | `ftp_command_handler` | ftp_hb_main | app_main.c | high | MQTT ftp_command topic dispatch handler (topic code 0x1000). Registered in the 12-byte MQT |
| `0x00015e74` | `spi_publish` | spi_bridge | spi_bridge.c | high | Core publish function: builds a SPI frame for a given control byte and channel, serialises |
| `0x00015f30` | `spi_publish_modem` | spi_bridge | spi_bridge.c | high | Sends a subscription announcement for a single channel ID on the modem-ctrl pipe (control  |
| `0x00015fa8` | `spi_subscribe_dispatch` | spi_bridge | spi_bridge.c | medium | Incoming subscribe-request dispatcher: decodes a CBOR map from i.MX8 containing a channel  |
| `0x00016410` | `spi_consumer_thread` | spi_bridge | spi_bridge.c | high | SPI slave receive loop: reads SPI frames from i.MX8, validates sync/length/CRC16, COBS-dec |
| `0x000169cc` | `spi_frame_encode` | spi_bridge | spi_bridge.c | high | SPI frame header builder: serialises control byte, channel tag, big-endian payload length, |
| `0x00016a50` | `spi_send_frame` | spi_bridge | spi_bridge.c | high | SPI message sender: COBS-encodes the built frame into the TX pipe, then calls spi_transcei |
| `0x00016bf8` | `spi_slave_init` | spi_bridge | spi_bridge.c | medium | SPI slave device initialisation: configures the nrfx_spis peripheral via the Zephyr SPI dr |
| `0x00016BF8` | `nrf_heartbeat_init` | ftp_hb_main | app_main.c | medium | Heartbeat subsystem initialisation: opens the WDT device via the Zephyr WDT driver vtable  |
| `0x00016CC4` | `heartbeat_publish` | ftp_hb_main | app_main.c | high | Periodic heartbeat MQTT publisher. No static callers — invoked by the k_timer expiry callb |
| `0x00016DA4` | `wdt_heartbeat_cb_setup` | ftp_hb_main | app_main.c | medium | Registers the WDT timeout callback with the nRF9160 WDT driver. Calls wdt_setup on the WDT |
| `0x00019788` | `modem_app_event_handler` | gnss_sms_time | cellular.c | medium | Main VanMoof modem application event handler — processes GNSS location fix events, publish |
| `0x0001af08` | `sms_event_dispatch` | gnss_sms_time | cellular.c | medium | SMS / modem event dispatcher — walks a linked list of registered handler entries (each sto |
| `0x0001e17c` | `sms_pdu_encode_gsm7` | gnss_sms_time | cellular.c | high | GSM 7-bit SMS PDU encoder — encodes a byte buffer into GSM septet-packed format, calling F |
| `0x0001f698` | `gnss_at_response_stub` | gnss_sms_time | cellular.c | low | Minimal wrapper that immediately tail-calls FUN_0001f368; in Ghidra the reachable block at |
| `0x000201a8` | `gnss_location_request` | gnss_sms_time | cellular.c | medium | GNSS / location request handler — iterates a list of location method structures; for each  |
| `0x000235ac` | `sms_send_pdu` | gnss_sms_time | cellular.c | high | VanMoof SMS send entry point — wraps the GSM 7-bit PDU encoder (sms_pdu_encode_gsm7) with  |
| `0x002FAF6` | `NMI_Handler` | ftp_hb_main | app_main.c | high | Zephyr main application thread entry. The nRF9160 MCUboot image stores this in the NMI vec |
| `0x00035FBA` | `app_init_array_runner` | ftp_hb_main | app_main.c | high | VanMoof application init array runner. Called between SYS_INIT level 2 and level 3 from NM |
| `0x0003706e` | `sms_send_dispatch` | gnss_sms_time | cellular.c | medium | Low-level SMS dispatch thunk — extracts the payload pointer and length from a message buff |
| `0x0003e094` | `cobs_encode_to_ring` | spi_bridge | spi_bridge.c | medium | COBS encoder: encodes a flat byte buffer into COBS format and appends the encoded bytes (i |
| `0x0003e162` | `cobs_decode_from_ring` | spi_bridge | spi_bridge.c | medium | COBS decoder: reads COBS-encoded bytes from the SPI input ring buffer and writes decoded p |

## Subsystem notes

### spi_bridge
Architecture: the nRF9160 modem firmware runs Zephyr RTOS with nRF Connect SDK. The SPI bridge subsystem connects the nRF9160 (SPI-slave, nrfx_spis driver) to the i.MX8M Nano (SPI-master). The i.MX8 side is the spi_mqtt_bridge service which uses control byte 0x81 for the modem-ctrl channel.

Two named threads appear in the rodata at 0x4289d / 0x428b1:
- \"spi_consumer_thread\" maps to FUN_00016410: the dominant 0x5BC-byte infinite-loop function that reads SPI frames, validates sync bytes (0x55/0xAA sentinel), checks CRC16 (FUN_0003404c = crc16_reflect), COBS-decodes via ring buffer (FUN_0003e162 at 0x3e162), enqueues decoded CBOR packets to a k_msgq (FUN_0003008c at 0x3008c), iterates a subscribe table calling spi_publish_modem (FUN_00015f30), then blocks on k_poll (FUN_000315b0 at 0x315b0) for the next SPI event.
- \"spi_process_thread\" maps to FUN_00012528: the Zephyr k_work_q thread body that dequeues submitted k_work items and dispatches their handler callbacks.

The outbound path (modem -> i.MX8):
  spi_publish (0x15e74) calls spi_frame_encode (0x169cc) to build the wire frame, then spi_send_frame (0x16a50) COBS-encodes it (FUN_0003e13e at 0x3e13e) and feeds it into the SPI T

### ppp_lte
## ppp_lte subsystem — nRF9160 modem firmware

### Image layout
The Ghidra program "modem.20240129.145222.1.5.0.main.v1.5.0-main.bin" is loaded at base 0x0. The nRF9160 MCUboot slot starts at runtime 0x23000, but Ghidra has not had set_image_base applied, so all absolute addresses in switch tables and log data are 0x23000-based runtime addresses while code lives at binary offsets 0x0–0x2FFFF. Strings live at binary 0x41000–0x4BFFF (Zephyr rodata region).

### Locating the functions
Because Zephyr's cbprintf/LOG_* system packages format-string pointers as PC-relative inline data (not traditional xrefs), get_xrefs_to on every string in the 0x41000–0x4BFFF region returned empty. The functions were located by:
1. Following the call graph forward from the Reset_Handler entry (0x2A148 → FUN_00029fc8) and then through the NMI_Handler slot that Ghidra incorrectly attributes to the main init orchestrator FUN_00013f3c.
2. String search via list_strings for "PPP", "send_data_thread", "recv_data_thread", "modem socket", "modem_lte_config" to identify the string set, then decompiling all functions in the 0x13000–0x16000 range where the MQTT dispatch and LTE/PPP logic lives.
3. Batch-decompiling

### gnss_sms_time
SUBSYSTEM ARCHITECTURE — gnss_sms_time (nRF9160 modem co-processor)

The VanMoof nRF9160 modem firmware (Zephyr RTOS + nRF Connect SDK, MCUboot image base 0x0, vector table at 0x200, image size 309132 bytes) implements three intertwined subsystems in one binary:

1. SMS (anti-theft): Outgoing SMS alerts are built as GSM 7-bit PDUs by sms_pdu_encode_gsm7 (0x1e17c) → sms_send_pdu (0x235ac) → sms_send_dispatch (0x3706e). Incoming SMS is routed through the sms_event_dispatch table (0x1af08). The Zephyr LOG_MODULE_REGISTER name for the SMS handler is "sms_callback" stored as a raw string at flash offset 0x41830; it is NOT referenced via any absolute 4-byte pointer in the binary because Zephyr with LTO inlines the log-source-const struct reference as a section-relative symbol resolved by the linker without generating a relocatable literal-pool entry.

2. GNSS/Location: gnss_location_request (0x201a8) iterates location method descriptors and dispatches to GNSS PVT, cell-based, or Wi-Fi positioning handlers. Results are consumed by modem_app_event_handler (0x19788) which logs the Google Maps URL string (0x41e93) and forwards coordinates to the i.MX8 main SoC via the SPI↔MQTT bridge. The nR

### ftp_hb_main
Architecture: Nordic nRF9160 (Cortex-M33, Thumb-2), Zephyr RTOS + nRF Connect SDK. MCUboot image loaded at flash base 0x28000; Ghidra image base is 0x0 so Ghidra address == file offset. Application vector table at file 0x200; reset vector at 0x204 → Reset_Handler 0x2A148 (Zephyr kernel). NMI vector at 0x208 → 0x2FAF6 = NMI_Handler, which Zephyr repurposes as the privileged main application thread entry.

How functions were located:
1. Vector table: NMI vector (0x208) → NMI_Handler @ 0x2FAF6 — confirmed as Zephyr main thread by its body (calls SYS_INIT runners, nrf_modem_lib_init, then the app connection loop).
2. Call graph: NMI_Handler → modem_main (FUN_00013f3c) → all VanMoof subsystem init functions. modem_main is the unique single-callee of NMI_Handler.
3. String tracing: Strings 'ftp_command', 'nrf_heartbeat', 'wdt_heartbeat_cb', 'Failed publishing heartbeat event', 'Failed publishing FTP data' are in rodata at file 0x41300+. They are NOT directly reachable by PC-relative LDR (>4KB gap from code section). Instead their flash-runtime addresses (file_offset + 0x28000) appear as literal pool values in code. Binary search found: flash 0x6a261 ('Failed publishing FTP data') literal
