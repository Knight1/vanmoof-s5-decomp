/*
 * auth.c — VanMoof BLE application layer.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions:
 *   auth_adv_extra_field_enabled       @ 0x0003d688
 *   auth_init_connection_table         @ 0x0003dfb8
 *   auth_copy_pubkey_32                @ 0x0003dff8
 *   auth_copy_bike_id                  @ 0x0003e020
 *   auth_submit_id_event               @ 0x0003e11c
 *   auth_alloc_disconnect_event        @ 0x0003e164
 *   auth_alloc_connection_event        @ 0x0003e178
 *   auth_check_connection_state        @ 0x0003e18c
 *   auth_format_ble_address            @ 0x0003e2d8
 *   auth_init_connection_slots         @ 0x0003e45c
 *   auth_send_disconnect_reason        @ 0x0003e4b0
 *   auth_alloc_event_0x14              @ 0x0003e574
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
void *auth_alloc_disconnect_event(void)
{
    void *blk = ble_event_alloc(0x18);
    if (blk != 0) {
        *(volatile uint32_t *)((uint8_t *)blk + 4) = (uint32_t)AUTH_EVT_DESC_DISCONNECT;
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
void *auth_alloc_connection_event(void)
{
    void *blk = ble_event_alloc(0x1c);
    if (blk != 0) {
        *(volatile uint32_t *)((uint8_t *)blk + 4) = (uint32_t)AUTH_EVT_DESC_CONNECTION;
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
        uint32_t idx = vm_conn_handle_to_index_446f4(conn);
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
    void *evt;
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
        *(volatile uint8_t *)((uint8_t *)evt + 0x08) = 1;
        *(volatile uint32_t *)((uint8_t *)evt + 0x0c) = conn_handle;
        *(volatile uint8_t *)((uint8_t *)evt + 0x10) = flag;
        ble_event_post();
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
void *auth_alloc_event_0x14(void)
{
    void *blk = ble_event_alloc(0x14);
    if (blk != 0) {
        *(volatile uint32_t *)((uint8_t *)blk + 4) = (uint32_t)AUTH_EVT_DESC_0X14;
    }
    return blk;
}
