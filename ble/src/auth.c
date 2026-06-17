/*
 * auth.c — VanMoof BLE authentication / secure-session layer.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   auth_adv_extra_field_enabled         @ 0x0003d688
 *   auth_handle_connection_command       @ 0x0003d6b0
 *   auth_send_connection_state           @ 0x0003d840
 *   auth_parse_certificate_challenge     @ 0x0003d920
 *   auth_init_connection_table           @ 0x0003dfb8
 *   auth_copy_pubkey_32                  @ 0x0003dff8
 *   auth_copy_bike_id                    @ 0x0003e020
 *   auth_submit_id_event                 @ 0x0003e11c
 *   auth_alloc_disconnect_event          @ 0x0003e164
 *   auth_alloc_connection_event          @ 0x0003e178
 *   auth_check_connection_state          @ 0x0003e18c
 *   auth_format_ble_address              @ 0x0003e2d8
 *   auth_handle_disconnect               @ 0x0003e350
 *   auth_handle_connect                  @ 0x0003e3a8
 *   auth_init_connection_slots           @ 0x0003e45c
 *   auth_send_disconnect_reason          @ 0x0003e4b0
 *   auth_alloc_event_0x14                @ 0x0003e574
 */

#include "ble.h"

/*
 * auth_adv_extra_field_enabled — should the advertising payload carry the extra
 * field. // 0x0003d688
 *
 * OEM disassembly (0x3d688..0x3d6a2):
 *
 * If either of the two state bytes is set the extra field is forced on (1);
 * otherwise it follows the low bit of the dedicated enable byte.
 */
uint8_t auth_adv_extra_field_enabled(void)
{
    uint8_t v;

    if (AUTH_ADV_FLAG_A == 0 && AUTH_ADV_FLAG_B == 0) {
        v = AUTH_ADV_EXTRA_FIELD;
    } else {
        v = 1;
    }
    return (uint8_t)(v & 1);
}

/*
 * auth_handle_connection_command - dispatch a parsed "connection command"
 * control message (subcommand 0..4) for the secure-session / pairing flow.
 * // 0x0003d6b0
 *
 * OEM disassembly (0x3d6b0..0x3d818):
 *
 * The message is laid out: +4 = static descriptor pointer (gates this handler),
 * +8 = subcommand byte, +0xc = flags word (bit0 = challenge variant, bit1 =
 * suppress state-message reply, bit2 = suppress findmy/enable publish on accept).
 * Subcommand 0 accepts a certificate challenge and emits a 0xc0 message;
 * 1 tears the secure session down; 2 (re)generates the local certificate;
 * 3 force-resets the session and queues a provisioning work item; 4 reboots.
 * Every non-rebooting path ends by sending a findmy state report.
 */
uint32_t auth_handle_connection_command(void *msg)
{
    const auth_conn_msg_t *m = (const auth_conn_msg_t *)msg;
    uint32_t flags = *(volatile uint32_t *)((const uint8_t *)msg + 0x0c);
    union {
        uint8_t b[0x80];
        void   *p[0x20];
    } frame;
    int rc;
    uint32_t reply;

    if (m->descriptor != AUTH_CONN_CMD_DESC) {
        return 0;
    }

    switch (m->command) {
    case 0:
        /* 3-byte challenge seed copied from rodata (0x62616 = {0x01,0x32,0x00}) */
        frame.b[0x0c] = AUTH_TIME_PUBKEY_SEED[0];
        frame.b[0x0d] = AUTH_TIME_PUBKEY_SEED[1];
        frame.b[0x0e] = AUTH_TIME_PUBKEY_SEED[2];
        if (auth_secure_state_get_4e9e4() != 0) {
            return 0;
        }
        if (flags & 1) {
            frame.b[0x0e] = 1;
            reply = 0xeb;
        } else {
            reply = 0xe9;
        }
        if (auth_certificate_challenge_apply_4e9fc(&frame.b[0x0c], AUTH_CERT_DESC) != 0) {
            return 0;
        }
        AUTH_ADV_EXTRA_FIELD = 1;
        if ((flags & 4) == 0) {
            ble_msg_publish_40558(AUTH_FINDMY_ENABLE_TOPIC, 0, 0);
        }
        AUTH_ADV_FLAG_A = 0;
        ble_json_writer_init(&frame.b[0x10], &frame.b[0x40], 0x40);
        ble_json_begin(&frame.b[0x20], &frame.b[0x10], 0);
        rc = ble_json_open(&frame.b[0x20], &frame.b[0x30], 0xffffffff);
        if (rc == 0) {
            if (auth_adv_extra_field_enabled() != 0) {
                ble_json_emit_key_587aa(&frame.b[0x20], AUTH_FINDMY_KEY);
                /* OEM passes r1 = the value left in r1 by ble_json_emit_key_587aa
                 * (decompiler 'extraout_r1'); with sign=0 the field value is the
                 * 0x01010000 constant, so the tag arg is the carried register. */
                ble_json_add_field_5fe92(&frame.b[0x20], 0, AUTH_FINDMY_FIELD_CONST, 0);
            }
            rc = ble_json_close(&frame.b[0x20], &frame.b[0x30]);
            if (rc == 0) {
                uint32_t len = *(volatile uint32_t *)((const uint8_t *)frame.p[0x08] + 4);
                ble_msg_transmit(0x80, 0xc0, &frame.b[0x40], len, 1, 0);
            }
        }
        break;

    case 1:
        if (auth_secure_state_get_4e9e4() == 0) {
            return 0;
        }
        if (auth_secure_session_teardown_4eb28() != 0) {
            return 0;
        }
        AUTH_ADV_EXTRA_FIELD = 0;
        AUTH_ADV_FLAG_A = 0;
        ble_msg_publish_40558(AUTH_FINDMY_ENABLE_TOPIC,
                              (const void *)&AUTH_ADV_EXTRA_FIELD, 1);
        reply = 0xea;
        break;

    case 2:
        if (auth_secure_state_get_4e9e4() == 0) {
            return 0;
        }
        if (auth_certificate_generate_4e418() != 0) {
            return 0;
        }
        AUTH_ADV_FLAG_A = 1;
        reply = 0xe6;
        break;

    case 3:
        if (auth_secure_state_get_4e9e4() != 0) {
            findmy_enqueue_event_1(2);
        }
        findmy_enqueue_event_0((flags & 2) ? 3 : 1);
        findmy_enqueue_event_1(2);
        auth_secure_session_reset_42ca8();
        ble_bus_publish_40618(0, AUTH_PROV_TOPIC_HANDLER, 0);
        {
            uint8_t *wi = (uint8_t *)findmy_alloc_work_item();
            if (wi != 0) {
                wi[8] = 4;
                *(uint32_t *)(wi + 0x0c) = 2;
                ble_event_post((int)wi);
            }
        }
        findmy_send_state_report();
        return 0;

    case 4:
        ble_system_reset_3ffac(1);
        /* does not return */

    default:
        findmy_send_state_report();
        return 0;
    }

    if ((flags & 2) == 0) {
        ble_send_state_msg_5876e(reply);
    }
    findmy_send_state_report();
    return 0;
}

/*
 * auth_send_connection_state - install the current connection-state descriptor,
 * drain any pending session records, push the cached serial / pub-key / bike-id
 * into the secure session, then emit a 0xe5 state message. // 0x0003d840
 *
 * OEM disassembly (0x3d840..0x3d8f6):
 *
 * Records are pumped until process_record returns 1 (done) or a negative error
 * (early return). The three session-field setters are chained: only when all
 * three succeed is the findmy-enabled flag (0x20007db6) raised. The persistent
 * secure flag at 0x20001189 is cleared + republished unless a rekey is pending
 * (0x20007db4 set), in which case a code-6 event is queued instead.
 */
void auth_send_connection_state(void)
{
    union {
        uint8_t b[0x6c];
        void   *p[0x1b];
    } frame;
    uint32_t count;
    int sel;

    auth_secure_session_install_4e298(AUTH_STATE_DESC);

    count = 2;
    findmy_read_records_433c0(&frame.b[0x28], &count);
    if (count < 2) {
        do {
            sel = findmy_process_record_433e4(0);
            if (sel < 0) {
                return;
            }
        } while (sel != 1);
    }

    if (auth_session_set_serial_4e8b8(AUTH_SESSION_SERIAL_SRC) == 0 &&
        auth_session_set_pubkey_4e8d4(AUTH_SESSION_PUBKEY_SRC) == 0 &&
        auth_session_set_bike_id_4e8f0(AUTH_SESSION_BIKEID_SRC) == 0) {
        AUTH_FINDMY_ENABLED_FLAG = 1;
    }

    ble_bus_publish_40618((uint32_t)AUTH_FINDMY_KEY, AUTH_STATE_REPORT_HANDLER, 0);

    if (AUTH_ADV_EXTRA_FIELD != 0) {
        if (AUTH_ADV_FLAG_B == 0) {
            AUTH_ADV_EXTRA_FIELD = 0;
            ble_msg_publish_40558(AUTH_FINDMY_ENABLE_TOPIC,
                                  (const void *)&AUTH_ADV_EXTRA_FIELD, 1);
        } else {
            findmy_enqueue_event_0(6);
        }
    }

    frame.b[0x28] = 0;
    frame.b[0x29] = 0;
    frame.b[0x2a] = 0;
    frame.b[0x2b] = 0;
    vm_memset_61e62(&frame.b[0x2c], 0, 0x3c);
    ble_json_writer_init(&frame.b[0x18], &frame.b[0x28], 0x40);
    ble_json_begin(&frame.b[0x08], &frame.b[0x18], 0);
    ble_json_add_state_58766(&frame.b[0x08], AUTH_FINDMY_ENABLED_FLAG);
    {
        uint32_t len = *(volatile uint32_t *)((const uint8_t *)frame.p[0x02] + 4);
        ble_msg_transmit(0x80, 0xe5, &frame.b[0x28], len, 1, 0);
    }
    findmy_send_state_report();
}

/*
 * auth_parse_certificate_challenge - verify a server certificate/challenge
 * message for a connection, latch the parsed fields into the connection slot,
 * and (on success) mark the link authenticated. // 0x0003d920
 *
 * OEM disassembly (0x3d920..0x3dafe):
 *
 * The challenge message is a 0x40-byte signature followed by a CBOR map. The
 * signature is checked against our stored server public key (AUTH_PUBKEY); the
 * map must open with entry tag 0xA0 and carries the certificate fields, which
 * are parsed into the per-connection slot (stride 0xb8 in AUTH_CONN_TABLE):
 * an issuer/version word at +8, a recognised DER OID at +0x1c, a 64-bit
 * validity-expiry at +0x10, a u32 at +0x18, a subject/command string at +0x2b
 * (with kind byte 'u'/'o' at +0x2a), and the 32-byte challenge/ephemeral key at
 * +0x50. The expiry is range-checked against the current time, and the cert
 * subject is required to match our own bike id. Any failure reports a
 * disconnect reason (1 parse/cert error, 2 sig/length error, 3 bike-id
 * mismatch, 4 expired/clock-invalid) via auth_send_disconnect_reason(conn,5,r).
 * On success the slot is flagged authenticated (state 2) and an internal auth
 * event is posted.
 */
void auth_parse_certificate_challenge(uint32_t conn_handle, const uint8_t *msg, uint32_t len)
{
    uint8_t *slot;
    uint32_t idx;
    int oid_len;
    int cmd_len;
    int id_cmp;
    uint32_t valid_lo, valid_hi;
    uint32_t now[2];
    uint8_t *evt;
    uint32_t reason;

    /* sp+0x20 (8 words): the value-reader struct; after cbor_get_now_ms() the
     * first two words are reinterpreted as the 64-bit "now" timestamp. */
    union {
        uint8_t  b[0x20];
        uint32_t w[8];
    } reader;
    uint8_t parse_out[12];   /* sp+0x04: CBOR parser scratch */
    uint8_t value_ctx[15];   /* sp+0x10: CBOR value ctx; entry tag at +0x0e */

    if (len <= 0x40) {
        reason = 2;
        goto fail;
    }

    idx = vm_conn_handle_to_index_446f4(conn_handle);
    slot = (uint8_t *)(idx * 0xb8u + AUTH_CONN_TABLE);
    if (slot[4] != 1) {
        return;
    }

    /* First 0x40 bytes are a signature over the remaining CBOR body, verified
     * against our stored server public key. */
    if (cbor_verify_signature(msg, msg + 0x40, len - 0x40, AUTH_VERIFY_PUBKEY) != 0) {
        reason = 2;
        goto fail;
    }

    idx = vm_conn_handle_to_index_446f4(conn_handle);
    slot = (uint8_t *)(idx * 0xb8u + AUTH_CONN_TABLE);
    vm_memset_61e62(slot + 8, 0, 0x68);

    settings_value_reader_init(&reader, (uint32_t)(uintptr_t)(msg + 0x40), len - 0x40);
    if (cbor_parser_init(&reader, 0, parse_out, value_ctx) != 0 ||
        value_ctx[0x0e] != 0xa0 ||
        cbor_map_get_u32(value_ctx, AUTH_CBOR_KEY_ISSUER, (uint32_t *)(slot + 8)) != 0) {
        reason = 1;
        goto fail;
    }

    /* Certificate OID: accept either of the two recognised DER OIDs. */
    oid_len = cbor_map_get_bstr_len(value_ctx, AUTH_CERT_OID_A, slot + 0x1c, 0xe);
    if (oid_len == -1) {
        oid_len = cbor_map_get_bstr_len(value_ctx, AUTH_CERT_OID_B, slot + 0x1c, 0xe);
        if (oid_len == -1) {
            reason = 1;
            goto fail;
        }
    }

    /* 64-bit validity-expiry into slot+0x10, and a u32 field into slot+0x18. */
    if (cbor_map_get_u64(value_ctx, AUTH_CBOR_KEY_AUTH_MODULE, (uint32_t *)(slot + 0x10)) != 0 ||
        cbor_map_get_u32(value_ctx, AUTH_CBOR_KEY_FMNA_SOUND, (uint32_t *)(slot + 0x18)) != 0) {
        reason = 1;
        goto fail;
    }

    /* Subject/command bstr into slot+0x2b, recording the kind byte at +0x2a. */
    cmd_len = cbor_map_get_bstr_len(value_ctx, AUTH_CBOR_KEY_CMD, slot + 0x2b, 0);
    if (cmd_len < 1) {
        cmd_len = cbor_map_get_bstr_len(value_ctx, AUTH_CBOR_KEY_CONNECTION_ID, slot + 0x2b, 0x25);
        if (cmd_len < 1) {
            reason = 1;
            goto fail;
        }
        slot[0x2a] = 0x6f;
    } else {
        slot[0x2a] = 0x75;
    }

    /* 32-byte challenge / ephemeral public key into slot+0x50. */
    if (cbor_map_get_bstr_len(value_ctx, AUTH_CBOR_KEY_FMNA_SERIAL, slot + 0x50, 0x20) == -1) {
        reason = 1;
        goto fail;
    }

    /* Expiry check: the slot+0x10 u64 is the validity end (seconds); convert to
     * ms and bound it against the current time and a fixed baseline. */
    idx = vm_conn_handle_to_index_446f4(conn_handle);
    slot = (uint8_t *)(idx * 0xb8u + AUTH_CONN_TABLE);
    valid_lo = *(uint32_t *)(slot + 0x10);
    valid_hi = *(uint32_t *)(slot + 0x14);

    if (cbor_get_now_ms(now) == 0) {
        int64_t now64  = (int64_t)(((uint64_t)now[1] << 32) | now[0]);
        int64_t expiry = (int64_t)((((uint64_t)valid_hi << 32) | valid_lo) * 1000ULL);
        if (now64 <= (int64_t)0x0000018cc251f3ffLL || now64 >= expiry) {
            reason = 4;
            goto fail;
        }
    }

    /* Certificate subject (slot+0x1c) must match our own bike id. */
    idx = vm_conn_handle_to_index_446f4(conn_handle);
    id_cmp = settings_strncmp((const char *)((uint8_t *)(idx * 0xb8u + AUTH_CONN_TABLE) + 0x1c),
                              (const char *)AUTH_BIKE_ID, 0xd);
    if (id_cmp != 0) {
        reason = 3;
        goto fail;
    }

    idx = vm_conn_handle_to_index_446f4(conn_handle);
    slot = (uint8_t *)(idx * 0xb8u + AUTH_CONN_TABLE);
    slot[4] = 2;

    evt = (uint8_t *)ble_event_alloc(0x10);
    if (evt == 0) {
        return;
    }
    *(uint32_t *)(evt + 4) = (uint32_t)(uintptr_t)AUTH_INTERNAL_EVENT_DESC;
    evt[8] = (uint8_t)id_cmp;
    *(uint32_t *)(evt + 0xc) = conn_handle;

    idx = vm_conn_handle_to_index_446f4(conn_handle);
    *(uint32_t *)(idx * 0xb8u + AUTH_CONN_TABLE) = conn_handle;
    ble_event_post((int)evt);
    return;

fail:
    auth_send_disconnect_reason(conn_handle, 5, reason);
}

/*
 * auth_init_connection_table — zero and initialise the connection slot table.
 * // 0x0003dfb8
 *
 * OEM disassembly (0x3dfb8..0x3dfec):
 *
 * Four slots of 0xb8 bytes each (loop runs until the word offset reaches 0xb8,
 * i.e. 0xb8 / 0x2e = 4 iterations). Per slot: clear the first word and the byte
 * at +4, memset the 0x68-byte body at +8, run the vendor sync-object init at
 * +0x80 with the fixed callback 0x000587e5, and plant a self-pointer at +0xb4.
 */
void auth_init_connection_table(void)
{
    uint8_t *slot = (uint8_t *)AUTH_CONN_TABLE;
    int off = 0;

    do {
        *(volatile uint32_t *)(slot + 0x00) = 0;
        *(volatile uint8_t  *)(slot + 0x04) = 0;
        vm_memset_61e62(slot + 0x08, 0, 0x68);
        vm_sync_object_init_61826(slot + 0x80, AUTH_CONN_SLOT_INIT_CB, 0);
        off += 0x2e;
        *(volatile uint32_t *)(slot + 0xb4) = (uint32_t)slot;
        slot += 0xb8;
    } while (off != 0xb8);
}

/*
 * auth_copy_pubkey_32 — store a 32-byte public key into the auth scratch.
 * // 0x0003dff8
 *
 * OEM disassembly (0x3dff8..0x3e01a):
 *
 * Clears the 32-byte public-key buffer then copies 32 bytes (eight words) from
 * the caller's source.
 */
void auth_copy_pubkey_32(const uint32_t *src)
{
    uint32_t *dst = (uint32_t *)AUTH_PUBKEY;
    const uint32_t *p = src;

    vm_memset_61e62((void *)AUTH_PUBKEY, 0, 0x20);
    do {
        *dst++ = *p++;
    } while (p != src + 8);
}

/*
 * auth_copy_bike_id — store the (up to 13-byte) bike id into the auth scratch.
 * // 0x0003e020
 *
 * OEM disassembly (0x3e020..0x3e052):
 *
 * For len <= 13: clear the 13-byte id buffer, then either copy `len` bytes from
 * the caller, or (when len == 0) seed it with the constant default "FACTORY"
 * (first eight bytes). Lengths above 13 are ignored.
 */
void auth_copy_bike_id(const void *src, uint32_t len)
{
    uint32_t *dst = (uint32_t *)AUTH_BIKE_ID;

    if (len < 0xe) {
        vm_memset_61e62((void *)AUTH_BIKE_ID, 0, 0xd);
        if (len != 0) {
            vm_memcpy_bounded_61e3c((void *)AUTH_BIKE_ID, src, len, 0xd);
            return;
        }
        dst[0] = ((const uint32_t *)AUTH_BIKE_ID_DEFAULT)[0];
        dst[1] = ((const uint32_t *)AUTH_BIKE_ID_DEFAULT)[1];
    }
}

/*
 * auth_submit_id_event — latch a (<=13-byte) id and post it on the auth event
 * queue. // 0x0003e11c
 *
 * OEM disassembly (0x3e11c..0x3e158):
 *
 * Rejects ids longer than 13 bytes (returns the source pointer untouched).
 * Otherwise it copies the id into the 13-byte event buffer, drops any held
 * queue lock, and tail-calls the vendor queue-post routine on the auth event
 * queue. (The decompiler's inlined critical-section / BASEPRI dance belongs to
 * that vendor routine.)
 */
uint32_t auth_submit_id_event(const void *src, uint32_t len)
{
    if (len > 0xd) {
        return (uint32_t)src;
    }
    vm_memset_61e62((void *)AUTH_ID_EVENT_BUF, 0, 0xd);
    vm_memcpy_bounded_61e3c((void *)AUTH_ID_EVENT_BUF, src, len, 0xd);
    if (vm_queue_lock_held_613de((void *)AUTH_EVENT_QUEUE) != 0) {
        vm_queue_lock_release_61490((void *)AUTH_EVENT_QUEUE);
    }
    return vm_queue_post_4fdd4((void *)AUTH_EVENT_QUEUE, 0, 0, 0);
}

/*
 * auth_alloc_disconnect_event - allocate a 0x18-byte auth event block and tag
 * it as a disconnect event. // 0x0003e164
 *
 * OEM disassembly (0x3e164..0x3e172):
 *
 * Asks the event allocator for an 18-hex-byte block. On success the descriptor
 * pointer at +4 is set to the static disconnect-event descriptor and the block
 * is returned (r0); on allocation failure NULL is returned untouched.
 */
int auth_alloc_disconnect_event(void)
{
    int blk = (int)ble_event_alloc(0x18);
    if (blk != 0) {
        *(volatile uint32_t *)(blk + 4) = (uint32_t)AUTH_EVT_DESC_DISCONNECT;
    }
    return blk;
}

/*
 * auth_alloc_connection_event - allocate a 0x1c-byte auth event block and tag
 * it as a connection event. // 0x0003e178
 *
 * OEM disassembly (0x3e178..0x3e186):
 *
 * Same shape as auth_alloc_disconnect_event but a larger (0x1c) block tagged
 * with the connection-event descriptor.
 */
int auth_alloc_connection_event(void)
{
    int blk = (int)ble_event_alloc(0x1c);
    if (blk != 0) {
        *(volatile uint32_t *)(blk + 4) = (uint32_t)AUTH_EVT_DESC_CONNECTION;
    }
    return blk;
}

/*
 * auth_check_connection_state — gate an operation on the connection's auth
 * state. // 0x0003e18c
 *
 * OEM disassembly (0x3e18c..0x3e1ae):
 *
 * Returns 0 when the link isn't ready, or when the per-connection slot flag at
 * +4 is already set; returns 6 (error code) when the link is ready but that
 * slot flag is clear.
 */
uint32_t auth_check_connection_state(uint32_t conn)
{
    uint32_t result;

    if (vm_link_ready_58858(conn) == 0) {
        result = 0;
    } else {
        uint32_t idx = vm_conn_handle_to_index_446f4((uint32_t)conn);
        if (*(volatile uint8_t *)(AUTH_CONN_STATE_TABLE + idx * 0x40 + 4) != 0) {
            result = 0;
        } else {
            result = 6;
        }
    }
    return result;
}

/*
 * auth_format_ble_address - render a 7-byte BLE address record into a human
 * readable string "AA:BB:CC:DD:EE:FF (type)". // 0x0003e2d8
 *
 * OEM disassembly (0x3e2d8..0x3e334):
 *
 * param_1 points to a 7-byte record: byte 0 = address type, bytes 1..6 = the
 * six address octets. The type selects a static name string ("public",
 * "random", "public-id", "random-id"); any other value is formatted as
 * "0x%02x". The six octets are then snprintf'd in reverse byte order into the
 * 30-byte output buffer (param_2) with the type name in parentheses.
 */
void auth_format_ble_address(const uint8_t *addr, char *out)
{
    char type_scratch[12];
    const char *type_name;

    switch (addr[0]) {
    case 0:
        type_name = AUTH_STR_PUBLIC;     /* "public"    */
        break;
    case 1:
        type_name = AUTH_STR_RANDOM;     /* "random"    */
        break;
    case 2:
        type_name = AUTH_STR_PUBLIC_ID;  /* "public-id" */
        break;
    case 3:
        type_name = AUTH_STR_RANDOM_ID;  /* "random-id" */
        break;
    default:
        ble_snprintf(type_scratch, 10, AUTH_STR_HEX_TYPE /* "0x%02x" */, addr[0]);
        goto format_out;
    }
    ble_strlcpy(type_scratch, type_name);
format_out:
    ble_snprintf(out, 0x1e, AUTH_STR_BLE_ADDR_FMT,
                 addr[6], addr[5], addr[4], addr[3], addr[2], addr[1],
                 type_scratch);
}

/*
 * auth_handle_disconnect - tear down the per-connection auth slot on a BLE
 * disconnect and post a disconnect event to the auth event queue.
 *
 * OEM disassembly (0x3e350 .. 0x3e3a2):
 *
 * If the connection is still valid we format its BLE address (kept for the
 * trace path inside auth_format_ble_address), stop the slot's connection
 * timer, drop our reference on the connection, then enqueue a disconnect
 * event carrying the connection pointer and HCI reason code.  // 0x3e350
 */
void auth_handle_disconnect(void *conn, uint8_t reason)
{
    int16_t idx;
    int event;
    uint8_t addr_buf[36];

    if (ble_conn_is_valid(conn) == 0)
        return;

    auth_format_ble_address((uint8_t *)ble_conn_get_address(conn), (char *)addr_buf);

    if (ble_conn_is_valid(conn) == 0)
        return;

    idx = (int16_t)vm_conn_handle_to_index_446f4((uint32_t)conn);
    ble_timer_stop(AUTH_CONN_STATE_TABLE + idx * 0x40 + 8);
    ble_conn_unref(conn);

    event = auth_alloc_connection_event();
    if (event != 0) {
        *(volatile uint8_t *)(event + 0x8) = 1;
        *(volatile uint32_t *)(event + 0xc) = (uint32_t)conn;
        *(volatile uint8_t *)(event + 0x10) = reason;
        ble_event_post(event);
    }
}

/*
 * auth_handle_connect - initialise the per-connection auth slot on a new BLE
 * connection, report the connection over the 0xfc0e control message, and post
 * a connect event to the auth event queue.
 *
 * OEM disassembly (0x3e3a8 .. 0x3e456):
 *
 * On a successful connect (status == 0) for a valid connection: send a
 * type-0xfc0e control report whose 4-byte body is {0x02, handle_lo, handle_hi,
 * 0x03}; bind the connection pointer into its auth slot, clear the slot's
 * state byte, start the slot connection timer (0x8000-tick period), take a
 * reference on the connection, and enqueue a connect event.  // 0x3e3a8
 */
void auth_handle_connect(void *conn, int status)
{
    uint16_t handle;
    int msg;
    uint8_t *body;
    unsigned int idx;
    int base;
    int slot;
    union {
        uint8_t  b[44];
        uint16_t h[22];
        uint32_t w[11];
    } frame;

    if (status != 0)
        return;

    auth_format_ble_address((uint8_t *)ble_conn_get_address(conn),
                            (char *)&frame.b[0x10]);

    if (ble_conn_is_valid(conn) == 0)
        return;

    /* tx_out slot pre-seeded with status (0); used as ble_msg_finalize out-param */
    frame.w[3] = (uint32_t)status;

    if (ble_conn_get_handle_id(conn, &handle) == 0) {
        msg = ble_msg_alloc(0xfc0e, 4);
        if (msg != 0) {
            body = (uint8_t *)ble_msg_reserve(msg + 8, 4);
            *(uint16_t *)(body + 1) = handle;
            body[0] = 2;
            body[3] = 3;
            if (ble_msg_finalize(0xfc0e, msg, (int *)&frame.w[3]) == 0)
                ble_msg_free((int)frame.w[3]);
        }
    }

    idx = vm_conn_handle_to_index_446f4((uint32_t)conn);
    base = AUTH_CONN_STATE_TABLE;
    slot = base + (int)idx * 0x40;
    *(volatile uint32_t *)slot = (uint32_t)conn;
    *(volatile uint8_t *)(slot + 4) = 0;
    ble_timer_init((int16_t)(idx * 0x40) + 8 + base, base, 0, 0, 0x8000, 0);
    ble_conn_ref(conn);

    msg = auth_alloc_connection_event();
    if (msg != 0) {
        *(volatile uint8_t *)(msg + 0x8) = 0;
        *(volatile uint32_t *)(msg + 0xc) = (uint32_t)conn;
        ble_event_post(msg);
    }
}

/*
 * auth_init_connection_slots - register the auth connection handlers and
 * initialise the four connection-slot records. // 0x0003e45c
 *
 * OEM disassembly (0x3e45c..0x3e494):
 *
 * Performs one-time auth bring-up: hands four RAM handler/argument blocks to
 * four vendor registration routines, then walks the four 0x40-byte connection
 * slots, initialising each slot's embedded object at +8 with slot_init_fn and
 * storing a self pointer at +0x3c.
 */
void auth_init_connection_slots(void)
{
    void *slot_init_fn = AUTH_SLOT_INIT_FN;
    uint8_t *slot = (uint8_t *)AUTH_CONN_SLOTS;
    int i;

    ble_reg_460d0((void *)AUTH_CONN_HANDLER_ARG);
    ble_reg_44978((void *)AUTH_REG_ARG_44978);
    ble_reg_44d5c((void *)AUTH_REG_ARG_44D5C);
    ble_reg_44d94((void *)AUTH_REG_ARG_44D94);

    i = 0;
    do {
        ble_obj_init_61826(slot + 8, slot_init_fn, 0);
        i++;
        *(volatile uint32_t *)(slot + 0x3c) = (uint32_t)slot;
        slot += 0x40;
    } while (i != 4);
}

/*
 * auth_send_disconnect_reason - build a JSON-ish "disconnect_reason" message,
 * transmit it, then queue a disconnect event. // 0x0003e4b0
 *
 * OEM disassembly (0x3e4b0..0x3e54c):
 *
 * Serialises a one-field object {"disconnect_reason": <text>} where the text
 * is chosen from the reason code (1..7); any other code yields "other". On a
 * clean serialise it sends the buffer as a 0x4002 message, then allocates a
 * disconnect event, fills in code=1 / conn handle / flag, and posts it.
 */
void auth_send_disconnect_reason(uint32_t conn_handle, uint8_t flag, uint32_t reason)
{
    int rc;
    const char *reason_str;
    int evt;
    void *writer[4];
    uint8_t state[16];
    uint8_t header[16];
    uint8_t buf[64];

    ble_json_writer_init(header, buf, 0x40);
    ble_json_begin(writer, header, 0);
    rc = ble_json_open(writer, state, 0xffffffff);
    if (rc != 0) {
        return;
    }

    ble_json_str(writer, AUTH_STR_DISCONNECT_REASON);

    reason_str = AUTH_STR_OTHER;
    switch (reason) {
    case 1:
        reason_str = AUTH_STR_INVALID_CERT;
        break;
    case 2:
        reason_str = AUTH_STR_SERVER_SIG_INVALID;
        break;
    case 3:
        reason_str = AUTH_STR_BIKE_ID_INVALID;
        break;
    case 4:
        reason_str = AUTH_STR_CERT_EXPIRED;
        break;
    case 5:
        reason_str = AUTH_STR_CERT_BLACKLISTED;
        break;
    case 6:
        reason_str = AUTH_STR_CHALLENGE_INVALID;
        break;
    case 7:
        reason_str = AUTH_STR_AUTH_TIMEOUT;
        break;
    default:
        break;
    }
    ble_json_str(writer, reason_str);

    rc = ble_json_close(writer, state);
    if (rc != 0) {
        return;
    }

    ble_msg_transmit(0x80, 0x4002, buf, *(volatile uint32_t *)((uint8_t *)writer[0] + 4), 0, 0);

    evt = auth_alloc_disconnect_event();
    if (evt != 0) {
        *(volatile uint8_t *)(evt + 0x08) = 1;
        *(volatile uint32_t *)(evt + 0x0c) = conn_handle;
        *(volatile uint8_t *)(evt + 0x10) = flag;
        ble_event_post(evt);
    }
}

/*
 * auth_alloc_event_0x14 - allocate a 0x14-byte auth event block and tag it with
 * the connect-state-machine event descriptor. // 0x0003e574
 *
 * OEM disassembly (0x3e574..0x3e582):
 *
 * Same allocate-and-tag pattern; smallest (0x14) block, tagged with the
 * third static event descriptor.
 */
int auth_alloc_event_0x14(void)
{
    int blk = (int)ble_event_alloc(0x14);
    if (blk != 0) {
        *(volatile uint32_t *)(blk + 4) = (uint32_t)AUTH_EVT_DESC_0X14;
    }
    return blk;
}
