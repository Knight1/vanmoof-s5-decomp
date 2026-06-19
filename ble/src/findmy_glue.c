/*
 * findmy_glue.c — VanMoof glue around the Apple Find My (FMNA) core.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   findmy_handle_conn_rx            @ 0x0003c6f4  (route peer RX -> FMNA message)
 *   findmy_conn_event_handler        @ 0x0003cabc  (conn/auth/sync events -> reports)
 *   findmy_forward_peer_payload      @ 0x0003cdcc  (forward a payload chunk)
 *   findmy_send_status_report        @ 0x0003ce20  (reset-reason report, cmd 0xc2)
 *   findmy_reset_conn_slots          @ 0x0003d134  (reset 3 FMNA conn records)
 *   findmy_msg_enqueue               @ 0x0003d160  (build + queue a 0x2f8 record)
 *   findmy_alloc_work_item           @ 0x0003d220  (alloc + tag a work item)
 *   findmy_match_provisioning_topic  @ 0x0003d234  (provisioning topic match)
 *   findmy_store_provisioning_token  @ 0x0003d29c  (store a <=16-byte token)
 *   findmy_send_state_report         @ 0x0003d47c  (state report, cmd 0xe1)
 *   findmy_build_message             @ 0x00058b12  (frame an FMNA transport message)
 *
 * This is VanMoof's own glue: routing between the BLE connection/peer layer and
 * the FMNA transport, CBOR/JSON serialization onto the comm bus, slot bookkeeping
 * and work-item plumbing. The Apple FMNA core itself is vendor (fmna-deferred)
 * and is not reconstructed.
 */

#include "ble.h"

/*
 * findmy_handle_conn_rx — route an inbound peer RX buffer to the FMNA transport
 * for one connection. // 0x0003c6f4
 *
 * OEM disassembly (0x0003c6f4..0x0003c758):
 *
 * Maps the connection handle to a slot index and indexes the 12-byte-per-entry
 * FINDMY_CONN_SLOTS table. If the slot's active byte (+0) is zero the connection
 * is not provisioned for FindMy, so it disconnects with reason 0xff. Otherwise,
 * for a payload of at least 2 bytes, it builds a framed FMNA message into
 * FINDMY_MSG_BUILD_BUF (sequence 0x2ee, the slot index as the type byte, channel
 * 0x20, the leading big-endian 16-bit word as the frame id, and the remaining
 * payload), clears the TX-busy flag, and sends it via ble_msg_send.
 */
void findmy_handle_conn_rx(uint32_t conn, const uint16_t *buf, uint32_t len)
{
    uint32_t idx;
    uint8_t *slot;
    int      built;

    idx = vm_conn_handle_to_index_446f4(conn);
    slot = (uint8_t *)(FINDMY_CONN_SLOTS + idx * 0xc);
    if (slot[0] == 0) {
        auth_send_disconnect_reason(*(uint32_t *)(slot + 8), 5, 0xff);
        return;
    }
    if (len > 1) {
        built = findmy_build_message((void *)FINDMY_MSG_BUILD_BUF, 0x2ee,
                                     (uint8_t)idx, 0x20,
                                     (uint16_t)((buf[0] << 8) | (buf[0] >> 8)),
                                     buf + 1, (int)(len - 2));
        if (built > 0) {
            ble_msg_tx_busy_clear();
            ble_msg_send((const void *)FINDMY_MSG_BUILD_BUF, (uint32_t)built);
        }
    }
}

/*
 * findmy_conn_event_handler — dispatch FMNA connection/auth/sync events and
 * serialize per-event reports. // 0x0003cabc
 *
 * OEM disassembly (0x0003cabc..0x0003cdcc):
 *
 * The event record carries a class-descriptor pointer at +4, a subcommand byte
 * at +8, a connection handle at +0xc, and event-specific fields beyond. It is
 * discriminated against three static class descriptors (conn/auth/sync).
 *
 * For a "conn_event" it switches on the subcommand: 0 (connect) clears the
 * slot's auth/enc bytes and stores the handle; 1 (disconnect) clears auth; 2
 * (rssi) emits {"cid","rssi"} and sends it raw (0xf200); 3 (security level)
 * derives enc = ((byte-2) < 2) then falls into 4; 4 re-derives the slot index;
 * 5 (conn params) emits {"latency","timeout","interval"} or {"err"} and enqueues
 * as type 7; 6 (data-length params) emits the tx/rx max fields or {"err"} and
 * enqueues as type 6.
 *
 * The trailing path: an "auth_event" subcommand 0 marks the slot authenticated
 * and stores +0x10 into slot.word4; a "sync_event" maps +0xc to a slot index.
 * When a valid slot results it emits {"enc","auth"} and enqueues as type 5.
 */
uint32_t findmy_conn_event_handler(const uint8_t *evt)
{
    uint8_t *slots = (uint8_t *)FINDMY_CONN_SLOTS;
    uint32_t idx = 0xff;
    void    *writer[4];         /* JSON writer state */
    uint8_t  json_state[16];
    uint8_t  begin_state[16];
    uint8_t  body[68];
    uint8_t *slot;
    uint32_t cmd_idx;
    int8_t   sbyte;

    if (*(const uint32_t *)(evt + 4) == FINDMY_EVT_DESC_CONN) {
        uint8_t sub = evt[8];
        if (sub <= 6) {
            switch (sub) {
            case 0:
                cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                slots[cmd_idx * 0xc] = 0;
                cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                slots[cmd_idx * 0xc + 1] = 0;
                cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                *(uint32_t *)(slots + cmd_idx * 0xc + 8) = *(const uint32_t *)(evt + 0xc);
                break;
            case 1:
                cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                slots[cmd_idx * 0xc] = 0;
                break;
            case 2:
                cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                sbyte = (int8_t)evt[0x10];
                ble_json_writer_init(begin_state, body, 0x40);
                ble_json_begin(writer, begin_state, 0);
                if (ble_json_open(writer, json_state, 0xffffffffu) == 0) {
                    ble_json_key_585a2(writer, FINDMY_KEY_CID);
                    ble_json_add_field_5fe92(writer, 0, cmd_idx, 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_RSSI);
                    ble_json_add_field_5fe92(writer, 0, (uint32_t)(int)sbyte,
                                             (uint32_t)((int)sbyte >> 31));
                    if (ble_json_close(writer, json_state) == 0) {
                        ble_msg_transmit(0x80, 0xf200, body,
                                         *(uint32_t *)((uint8_t *)writer[0] + 4), 0, 0);
                    }
                }
                break;
            case 3:
                if (evt[0x11] == 0) {
                    sbyte = (int8_t)evt[0x10];
                    cmd_idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                    slots[cmd_idx * 0xc + 1] = (uint8_t)((uint8_t)((uint8_t)sbyte - 2) < 2);
                    idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                }
                break;
            case 4:
                idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
                break;
            case 5:
                ble_json_writer_init(begin_state, body, 0x40);
                ble_json_begin(writer, begin_state, 0);
                if (ble_json_open(writer, json_state, 0xffffffffu) != 0)
                    return 0;
                if (*(const int *)(evt + 0x10) == 0) {
                    ble_json_key_585a2(writer, FINDMY_KEY_LATENCY);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x16), 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_TIMEOUT);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x18), 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_INTERVAL);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x14), 0);
                } else {
                    uint32_t v = *(const uint32_t *)(evt + 0x10);
                    ble_json_key_585a2(writer, FINDMY_KEY_ERR);
                    ble_json_add_field_5fe92(writer, 0, v, (uint32_t)((int)v >> 31));
                }
                if (ble_json_close(writer, json_state) != 0)
                    return 0;
                findmy_msg_enqueue(*(const uint32_t *)(evt + 0xc), 7, body,
                                   *(const uint16_t *)((uint8_t *)writer[0] + 4));
                break;
            case 6:
                ble_json_writer_init(begin_state, body, 0x40);
                ble_json_begin(writer, begin_state, 0);
                if (ble_json_open(writer, json_state, 0xffffffffu) != 0)
                    return 0;
                if (*(const int *)(evt + 0x10) == 0) {
                    ble_json_key_585a2(writer, FINDMY_KEY_TX_MAX_TIME);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x16), 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_TX_MAX_LEN);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x14), 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_RX_MAX_TIME);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x1a), 0);
                    ble_json_key_585a2(writer, FINDMY_KEY_RX_MAX_LEN);
                    ble_json_add_field_5fe92(writer, 0, *(const uint16_t *)(evt + 0x18), 0);
                } else {
                    uint32_t v = *(const uint32_t *)(evt + 0x10);
                    ble_json_key_585a2(writer, FINDMY_KEY_ERR);
                    ble_json_add_field_5fe92(writer, 0, v, (uint32_t)((int)v >> 31));
                }
                if (ble_json_close(writer, json_state) != 0)
                    return 0;
                findmy_msg_enqueue(*(const uint32_t *)(evt + 0xc), 6, body,
                                   *(const uint16_t *)((uint8_t *)writer[0] + 4));
                break;
            }
        }
    }

    if (*(const uint32_t *)(evt + 4) == FINDMY_EVT_DESC_AUTH) {
        if (evt[8] == 0) {
            uint32_t conn = *(const uint32_t *)(evt + 0xc);
            slot = slots + conn * 0xc;
            slot[0] = 1;
            *(uint32_t *)(slot + 4) = *(const uint32_t *)(evt + 0x10);
            idx = conn & 0xff;
        }
    } else if (*(const uint32_t *)(evt + 4) == FINDMY_EVT_DESC_SYNC) {
        idx = vm_conn_handle_to_index_446f4(*(const uint32_t *)(evt + 0xc));
    }

    if (idx != 0xff) {
        ble_json_writer_init(begin_state, body, 0x40);
        ble_json_begin(writer, begin_state, 0);
        if (ble_json_open(writer, json_state, 0xffffffffu) == 0) {
            slot = slots + idx * 0xc;
            ble_json_key_585a2(writer, FINDMY_KEY_ENC);
            ble_json_add_field_5feae(writer, (uint8_t)(slot[1] + 0x14));
            ble_json_key_585a2(writer, FINDMY_KEY_AUTH);
            ble_json_add_field_5feae(writer, (uint8_t)(slot[0] + 0x14));
            if (ble_json_close(writer, json_state) == 0) {
                findmy_msg_enqueue(*(uint32_t *)(slot + 8), 5, body,
                                   *(const uint16_t *)((uint8_t *)writer[0] + 4));
            }
        }
    }
    return 0;
}

/*
 * findmy_forward_peer_payload — forward a peer payload chunk to the FMNA
 * transport for the matching slot. // 0x0003cdcc
 *
 * OEM disassembly (0x0003cdcc..0x0003ce18):
 *
 * The inbound record's first byte is the slot index. If that slot is active it
 * stages a frame in FINDMY_FWD_SCRATCH: the 16-bit field at rec+1 written
 * big-endian into scratch[0..1], then the payload (rec+10, length = word at
 * rec+4, capped at 0x2ec) copied to scratch+2. It then enqueues the staged frame
 * as type 1 keyed on the slot's connection handle. Inactive slots are dropped.
 */
void findmy_forward_peer_payload(const uint8_t *rec)
{
    uint8_t *slots = (uint8_t *)FINDMY_CONN_SLOTS;
    uint8_t *scratch = (uint8_t *)FINDMY_FWD_SCRATCH;
    uint16_t hdr;

    if (slots[(uint32_t)rec[0] * 0xc] != 0) {
        hdr = *(const uint16_t *)(rec + 1);
        scratch[1] = (uint8_t)hdr;
        scratch[0] = (uint8_t)(hdr >> 8);
        vm_memcpy_bounded_61e3c(scratch + 2, rec + 10,
                                *(const uint32_t *)(rec + 4), 0x2ec);
        findmy_msg_enqueue(*(uint32_t *)(slots + (uint32_t)rec[0] * 0xc + 8), 1,
                           scratch,
                           (uint16_t)((*(const uint32_t *)(rec + 4) + 2) & 0xffff));
    }
}

/*
 * findmy_send_status_report — build and send an FMNA status report carrying the
 * hardware reset reason (command 0xc2). // 0x0003ce20
 *
 * OEM disassembly (0x0003ce20..0x0003ce8c):
 *
 * Opens a JSON object; on success reads POWER->RESETREAS into a local word and
 * clears it, emits {"reason": (signed)reason}, closes the object, and sends the
 * body via ble_msg_transmit(0x80, 0xc2, ..., 0, 1) — the trailing 1 marks a
 * reliable/acknowledged send.
 */
void findmy_send_status_report(void)
{
    void    *writer[4];
    uint8_t  json_state[16];
    uint8_t  begin_state[16];
    uint8_t  body[68];
    int      reason;

    ble_json_writer_init(begin_state, body, 0x40);
    ble_json_begin(writer, begin_state, 0);
    if (ble_json_open(writer, json_state, 0xffffffffu) == 0) {
        ble_resetreas_read_5f34e((uint32_t *)&reason);
        ble_resetreas_clear_5f388();
        ble_json_key_585a2(writer, FINDMY_KEY_REASON);
        ble_json_add_field_5fe92(writer, 0, (uint32_t)reason,
                                 (uint32_t)(reason >> 31));
        if (ble_json_close(writer, json_state) == 0) {
            ble_msg_transmit(0x80, 0xc2, body,
                             *(uint32_t *)((uint8_t *)writer[0] + 4), 0, 1);
        }
    }
}

/*
 * findmy_reset_conn_slots — reset the three FMNA connection-record slots to their
 * idle baseline. // 0x0003d134
 *
 * OEM disassembly (0x0003d134..0x0003d15a):
 *
 * Iterates three times over the fixed-size 0x338-byte records based at
 * FINDMY_CONN_SLOT_BASE. For each it re-initialises the embedded queue/sync
 * object (ble_obj_init_61826(slot, 0, 0)), stamps the byte at slot-0x8 (the
 * record base) with 0x80 and the byte at slot+0x39 with 0xff, then advances by
 * 0x338. No return value.
 */
void findmy_reset_conn_slots(void)
{
    uint8_t *slot = (uint8_t *)FINDMY_CONN_SLOT_BASE;
    int i = 0;

    do {
        i = i + 1;
        ble_obj_init_61826(slot, (void *)0, 0);
        slot[-0x8] = 0x80;
        slot[0x39] = 0xff;
        slot += 0x338;
    } while (i != 3);
}

/*
 * findmy_msg_enqueue — build a 0x2f8-byte FindMy message record on the stack and
 * post it to the message queue. // 0x0003d160
 *
 * OEM disassembly (0x0003d160..0x0003d1be):
 *
 * Rejects with -EINVAL (-0x16) unless 1 <= len <= 0x2ee. Otherwise it zero-fills
 * a 0x2f8-byte record, lays out {tag@+0, kind@+4, payload@+5 (cap 0x2f3),
 * len@+0x2f4}, and posts it to the queue handle at FINDMY_MSG_QUEUE_ADDR via the
 * vendor msgq-put primitive. Returns 0 on success or -12 on a send failure.
 */
uint32_t findmy_msg_enqueue(uint32_t tag, uint8_t kind, const void *src, uint16_t len)
{
    uint8_t rec[0x2f8];

    if (((uint32_t)(len - 1U) & 0xffff) >= 0x2ee)
        return 0xffffffeau; /* -EINVAL */

    vm_memset_61e62(rec, 0, 0x2f8);
    *(uint16_t *)(rec + 0x2f4) = len;
    *(uint32_t *)(rec + 0x0) = tag;
    rec[0x4] = kind;
    vm_memcpy_bounded_61e3c(rec + 5, src, len, 0x2f3);

    if (comm_msgq_put_4f318((void *)*(volatile uint32_t *)FINDMY_MSG_QUEUE_ADDR,
                            rec, 0, 0) != 0)
        return 0xfffffff4u; /* -12 on send failure */
    return 0;
}

/*
 * findmy_alloc_work_item — allocate a 0x10-byte FindMy work item and stamp its
 * descriptor word. // 0x0003d220
 *
 * OEM disassembly (0x0003d220..0x0003d22e):
 *
 * Calls ble_event_alloc(0x10); on success it stores the connection-command
 * descriptor pointer (AUTH_CONN_CMD_DESC) into the work item at +0x4. Returns the
 * allocated pointer (NULL on failure).
 */
void *findmy_alloc_work_item(void)
{
    void *wi = ble_event_alloc(0x10);
    if (wi != 0)
        *(uint32_t *)((char *)wi + 4) = AUTH_CONN_CMD_DESC;
    return wi;
}

/*
 * findmy_match_provisioning_topic — test a bus/MQTT topic against the fmna
 * provisioning-topic table and, on a match, re-publish it with no payload.
 * Registered as a bus subscriber (also AUTH_PROV_TOPIC_HANDLER). // 0x0003d234
 *
 * OEM disassembly (0x0003d234..0x0003d292):
 *
 * Walks the 3-entry runtime table FINDMY_PROV_TOPIC_TABLE. For each entry it
 * picks the longer of the incoming topic and the table entry as the compare
 * length and runs a bounded compare of the whole topic against the entry; on a
 * body match it then requires the topic tail (4 bytes at topic+len-2) to equal
 * the "/1" slot suffix. On a full match it re-publishes the topic with a NULL
 * payload and returns 0; no match returns 0 without publishing.
 */
void *findmy_match_provisioning_topic(const char *topic, uint32_t len)
{
    const char *const *table = (const char *const *)FINDMY_PROV_TOPIC_TABLE;
    const char *suffix = FINDMY_PROV_TOPIC_SUFFIX;   /* "/1" */
    const char *tail = topic + len - 2;
    int i = 3;

    for (;;) {
        const char *entry = table[0];
        uint32_t topic_len = vm_strlen_36d1c(topic);
        const char *longer = topic;
        uint32_t entry_len = vm_strlen_36d1c(*table);
        if (entry_len <= topic_len) {
            longer = *table;
        }
        ++table;
        {
            uint32_t cmp_len = vm_strlen_36d1c(longer);
            if (settings_strncmp(topic, entry, (int)cmp_len) == 0 &&
                settings_strncmp(tail, suffix, 4) == 0) {
                break;
            }
        }
        if (--i == 0) {
            return 0;
        }
    }

    ble_msg_publish_clear_59bac(topic);
    return 0;
}

/*
 * findmy_store_provisioning_token — pull a provisioning token (a CBOR byte-string
 * field, at most 16 bytes) out of the inbound message and re-publish it on the
 * in-process "vm" bus topic. // 0x0003d29c
 *
 * OEM disassembly (0x0003d29c..0x0003d2d6):
 *
 * Extracts the CBOR byte-string into an 0x80-byte scratch (max 0x80). If the
 * length is <= 0x10 it zeroes a 16-byte buffer, copies `len` bytes of the token
 * in, and publishes the fixed 0x10-byte buffer on the "vm" bus topic. Oversized
 * tokens are dropped. The leading command-handler args a0/a1/a2 are unused.
 */
void findmy_store_provisioning_token(uint32_t a0, uint32_t a1, uint32_t a2,
                                     uint32_t src_a, uint32_t src_b)
{
    uint8_t  scratch[0x80];
    uint8_t  token[0x10];
    uint32_t len;

    (void)a0;
    (void)a1;
    (void)a2;

    len = findmy_cbor_get_bstr_58cd8(scratch, 0x80, src_a, src_b);
    if (len <= 0x10) {
        token[0] = 0; token[1] = 0; token[2] = 0; token[3] = 0;
        token[4] = 0; token[5] = 0; token[6] = 0; token[7] = 0;
        token[8] = 0; token[9] = 0; token[10] = 0; token[11] = 0;
        token[12] = 0; token[13] = 0; token[14] = 0; token[15] = 0;
        vm_memcpy_bounded_61e3c(token, scratch, len, len);
        ble_msg_publish_40558(BLE_BUS_TOPIC_VM, token, 0x10);
    }
}

/*
 * findmy_send_state_report — serialize the FindMy state as a JSON object and
 * transmit it as command 0xe1. // 0x0003d47c
 *
 * OEM disassembly (0x0003d47c..0x0003d520):
 *
 * Opens a JSON object and, on success, emits five key/value pairs (each via
 * ble_json_emit_key_587aa + ble_json_add_state_58766): "ready" <-
 * auth_secure_state_get_4e9e4(), "enabled" <- AUTH_ADV_EXTRA_FIELD, "paired" <-
 * AUTH_ADV_FLAG_B, "pairing" <- AUTH_ADV_FLAG_A, "provisioned" <-
 * AUTH_FINDMY_ENABLED_FLAG. It closes the object and, on success, transmits the
 * buffer via ble_msg_transmit(0x80, 0xe1, ..., 1, 1).
 */
void findmy_send_state_report(void)
{
    uint32_t json_state[4];
    uint8_t  close_ctx[16];
    uint8_t  hdr[16];
    uint8_t  out[68];

    ble_json_writer_init(hdr, out, 0x40);
    ble_json_begin(json_state, hdr, 0);
    if (ble_json_open(json_state, close_ctx, 0xffffffffu) == 0) {
        ble_json_emit_key_587aa(json_state, FINDMY_STATE_KEY_READY);
        ble_json_add_state_58766(json_state, (uint8_t)auth_secure_state_get_4e9e4());
        ble_json_emit_key_587aa(json_state, FINDMY_STATE_KEY_ENABLED);
        ble_json_add_state_58766(json_state, AUTH_ADV_EXTRA_FIELD);
        ble_json_emit_key_587aa(json_state, FINDMY_STATE_KEY_PAIRED);
        ble_json_add_state_58766(json_state, AUTH_ADV_FLAG_B);
        ble_json_emit_key_587aa(json_state, FINDMY_STATE_KEY_PAIRING);
        ble_json_add_state_58766(json_state, AUTH_ADV_FLAG_A);
        ble_json_emit_key_587aa(json_state, FINDMY_STATE_KEY_PROVISIONED);
        ble_json_add_state_58766(json_state, AUTH_FINDMY_ENABLED_FLAG);
        if (ble_json_close(json_state, close_ctx) == 0) {
            ble_msg_transmit(0x80, 0xe1, out,
                             *(uint32_t *)(json_state[0] + 4), 1, 1);
        }
    }
}

/*
 * findmy_build_message — frame an FMNA transport message into `buf`. // 0x00058b12
 *
 * OEM disassembly (0x00058b12..0x00058b86):
 *
 * Rejects payloads whose framed size (payload_len + 10) exceeds 0x400, returning
 * -1. Otherwise it lays out a 10-byte header followed by the payload:
 *   +0x0 type, +0x1..2 frame_id, +0x3 chan, +0x4..7 payload_len,
 *   +0x8..9 CRC-16 (seed 0xffff over the payload), +0xa.. payload.
 * After copying the payload it byte-swaps the multi-byte header fields
 * (frame_id, payload_len and the CRC) to big-endian. Returns the framed length.
 * The `seq` argument is part of the ABI but unused by the body.
 */
int findmy_build_message(void *buf, uint32_t seq, uint8_t type, uint8_t chan,
                         uint16_t frame_id, const void *payload, int payload_len)
{
    uint8_t *b = (uint8_t *)buf;
    uint32_t total = (uint32_t)(payload_len + 10);
    uint32_t crc;
    uint32_t plen;
    uint16_t fid;

    (void)seq;

    if (total >= 0x401) {
        return -1;
    }

    b[3] = chan;
    *(uint16_t *)(b + 1) = frame_id;
    b[0] = type;
    *(uint32_t *)(b + 4) = (uint32_t)payload_len;
    crc = comm_crc16_58d7c(0xffff, payload, (uint32_t)payload_len);
    *(uint16_t *)(b + 8) = (uint16_t)crc;
    vm_memcpy_61e20(b + 10, payload, (uint32_t)payload_len);

    /* byte-swap the multi-byte header fields to big-endian */
    fid = *(uint16_t *)(b + 1);
    *(uint16_t *)(b + 1) = (uint16_t)((fid << 8) | (fid >> 8));
    plen = *(uint32_t *)(b + 4);
    *(uint32_t *)(b + 4) = (plen << 24) | ((plen >> 8 & 0xff) << 16) |
                           ((plen >> 16 & 0xff) << 8) | (plen >> 24);
    *(uint16_t *)(b + 8) = (uint16_t)(((crc & 0xff) << 8) | ((crc >> 8) & 0xff));

    return (int)total;
}
