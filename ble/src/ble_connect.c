/*
 * ble_connect.c — VanMoof BLE connect / advertising state.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   ble_connect_clear_adv_flag         @ 0x0003e588  (clear connect-state byte)
 *   ble_connect_set_ready_flag         @ 0x0003e59c  (set connect-state byte = 1)
 *   ble_send_signed_connect_payload    @ 0x0003e5b0
 *   ble_connect_state_machine          @ 0x0003e640
 *   ble_advertise_bike_id_payload      @ 0x0003e6e0
 *   ble_advertise_ecu_serial_payload   @ 0x0003e72c
 *   ble_secure_session_init            @ 0x0003e778
 *   ble_build_connect_advert_payload   @ 0x0003e948
 *
 * The first two gate the single connect/advertising state byte
 * (BLE_CONNECT_STATE, 0x20007ef5) on a vendor link-state check, only updating it
 * when the check returns 0. The rest build/sign the connect notification payload
 * and drive the pairing handshake FSM.
 */

#include "ble.h"

/*
 * ble_connect_clear_adv_flag — clear the connect/advertising state. // 0x0003e588
 *
 * OEM disassembly (0x3e588..0x3e594):
 *
 * The store reuses r0 (which is 0 when the branch is not taken), i.e. it writes
 * the literal 0 — reproduced here as an explicit `= 0`.
 */
void ble_connect_clear_adv_flag(void)
{
    if (ble_link_check_51b9c() == 0) {
        BLE_CONNECT_STATE = 0;
    }
}

/*
 * ble_connect_set_ready_flag — mark the connect state ready. // 0x0003e59c
 *
 * OEM disassembly (0x3e59c..0x3e5aa):
 */
void ble_connect_set_ready_flag(void)
{
    if (ble_link_check_51b7c() == 0) {
        BLE_CONNECT_STATE = 1;
    }
}

/*
 * ble_send_signed_connect_payload — build a COSE/CWT-signed "connect" payload
 * and push it out over the connect GATT characteristic. // 0x0003e5b0
 *
 * OEM disassembly (0x0003e5b0..0x0003e62e):
 *
 * If the advertising-ready flag (BLE_CONNECT_STATE) is set, the advertising flag
 * is first cleared via ble_connect_clear_adv_flag(). A variable-size scratch
 * buffer is alloca'd (rounded up to a multiple of 8, large enough for
 * prefix_len + data_len) and the composed string "prefix||data" is built in it
 * (prefix copied first, data after). The vendor EXT_API COSE signer is invoked
 * with the audience string "nl.samsonit.vanmoofapp" (and its strlen), the
 * composed buffer, the total length, the connect message-slot handle
 * (BLE_CONNECT_MSG_HANDLE, used directly as the slot id) and a length word
 * (seeded to 0x100) — six args in all; it writes the signed blob into that slot.
 * On success the GATT length-header reserve and the EXT_API send/commit helpers
 * are called against (handle - 2) with that length word; when all three succeed
 * the advertising-ready flag is re-armed.
 */
void ble_send_signed_connect_payload(const void *data, int data_len,
                                     const void *prefix, int prefix_len)
{
    uint8_t *buf;
    uint32_t total;
    uint32_t aligned;
    int      handle;
    int      rc;
    uint32_t len;

    if (BLE_CONNECT_STATE != 0) {
        ble_connect_clear_adv_flag();
    }

    total   = (uint32_t)(prefix_len + data_len);
    aligned = (total + 7u) & 0xfffffff8u;
    buf     = (uint8_t *)__builtin_alloca(aligned);

    /* compose: prefix first, then the data */
    vm_memcpy_61e20(buf, prefix, (uint32_t)prefix_len);
    vm_memcpy_61e20(buf + prefix_len, data, (uint32_t)data_len);

    len    = 0x100;
    handle = BLE_CONNECT_MSG_HANDLE;

    /* sign "prefix||data" into the message slot, audience = the app id; the slot
     * handle and the length word (seeded to 0x100) are passed as the 5th/6th args */
    rc = ble_cose_sign_4a7ec(BLE_COSE_AUDIENCE,
                             (int)vm_strlen_36d1c(BLE_COSE_AUDIENCE),
                             buf, (int)total, handle, &len);
    if (rc == 0 &&
        (rc = ble_msg_reserve_len_59e16(handle - 2, &len)) == 0 &&
        (rc = ble_msg_send_51670(handle - 2, len)) == 0) {
        ble_connect_set_ready_flag();
    }
}

/*
 * ble_connect_state_machine — pairing/connect finite-state machine fed a control
 * message whose mode byte (+8) advances the connect handshake. // 0x0003e640
 *
 * OEM disassembly (0x0003e640..0x0003e6ca):
 *
 * The message descriptor word (msg+4) is first checked against the static
 * connection-event descriptor AUTH_EVT_DESC_0X14; a mismatch traps (udf). The
 * mode byte (msg+8) then drives the persistent FSM mode (BLE_CONNECT_FSM_MODE):
 * mode 3 sets it to 1 (begin); mode 4 advances 1 -> 2; mode 2, only when the
 * mode is already 2, sends the signed connect payload (bike_id URL prefix + the
 * staged connect buffer), clears the mode to 0, and emits a small JSON status
 * message (type/sub 0x80/0xfa). Any other mode is a no-op. Always returns 1.
 */
uint32_t ble_connect_state_machine(void *msg)
{
    uint8_t  mode;
    uint8_t  json_writer[16];
    uint8_t  json_buf[68];
    void    *json_state[4];
    uint32_t len;

    if (*(uint32_t *)((uint8_t *)msg + 4) != (uint32_t)AUTH_EVT_DESC_0X14) {
        __builtin_trap();
    }

    mode = *((uint8_t *)msg + 8);
    if (mode == 3) {
        BLE_CONNECT_FSM_MODE = 1;
    } else if (mode == 4) {
        if (BLE_CONNECT_FSM_MODE == 1) {
            BLE_CONNECT_FSM_MODE = 2;
        }
    } else if (mode == 2) {
        if (BLE_CONNECT_FSM_MODE != 2) {
            return 1;
        }
        len = BLE_CONNECT_PAYLOAD_LEN;
        ble_send_signed_connect_payload(BLE_CONNECT_PAYLOAD_BUF, (int)len,
                                        BLE_CONNECT_URL_BIKE_ID,
                                        (int)vm_strlen_36d1c(BLE_CONNECT_URL_BIKE_ID));
        BLE_CONNECT_FSM_MODE = 0;

        ble_json_writer_init(json_writer, json_buf, 0x40);
        ble_json_begin(json_state, json_writer, 0);
        ble_json_open_5fe82(json_state, 0, 1, 0);
        ble_msg_transmit(0x80, 0xfa, json_buf,
                         *(uint32_t *)((uint8_t *)json_state[0] + 4), 0, 0);
    }
    return 1;
}

/*
 * ble_advertise_bike_id_payload — stage a payload into the 32-byte connect
 * buffer, record its length, and send it COSE-signed. // 0x0003e6e0
 *
 * OEM disassembly (0x0003e6e0..0x0003e71a):
 *
 * Stores len to the staged-payload length word, zeroes the 32-byte connect
 * buffer, copies up to 32 bytes from src into it, then tail-calls
 * ble_send_signed_connect_payload with (src, len) and the URL prefix.
 *
 * NOTE: this OEM-named variant actually loads the "main_ecu_serial" URL prefix;
 * its caller (ble_build_connect_advert_payload) feeds it the ecu_serial buffer,
 * so prefix and content match. The bike_id/ecu_serial names in this pair are
 * crossed relative to the URL each emits (see ble_advertise_ecu_serial_payload).
 */
void ble_advertise_bike_id_payload(const void *src, uint32_t len)
{
    BLE_CONNECT_PAYLOAD_LEN = len;
    vm_memset_61e62(BLE_CONNECT_PAYLOAD_BUF, 0, 0x20);
    vm_memcpy_bounded_61e3c(BLE_CONNECT_PAYLOAD_BUF, src, len, 0x20);
    ble_send_signed_connect_payload(src, (int)len, BLE_CONNECT_URL_ECU,
                                    (int)vm_strlen_36d1c(BLE_CONNECT_URL_ECU));
}

/*
 * ble_advertise_ecu_serial_payload — stage a payload into the 32-byte connect
 * buffer, record its length, and send it COSE-signed. // 0x0003e72c
 *
 * OEM disassembly (0x0003e72c..0x0003e766):
 *
 * Identical structure to ble_advertise_bike_id_payload but loads the "bike_id"
 * URL prefix; its caller feeds it the bike_id buffer, so prefix and content
 * match. (Names in this pair are crossed relative to the emitted URL.)
 */
void ble_advertise_ecu_serial_payload(const void *src, uint32_t len)
{
    BLE_CONNECT_PAYLOAD_LEN = len;
    vm_memset_61e62(BLE_CONNECT_PAYLOAD_BUF, 0, 0x20);
    vm_memcpy_bounded_61e3c(BLE_CONNECT_PAYLOAD_BUF, src, len, 0x20);
    ble_send_signed_connect_payload(src, (int)len, BLE_CONNECT_URL_BIKE_ID,
                                    (int)vm_strlen_36d1c(BLE_CONNECT_URL_BIKE_ID));
}

/*
 * ble_secure_session_init — application entry point that brings up the VanMoof
 * secure-channel session by tail-calling the vendor session-init routine.
 * // 0x0003e778
 *
 * OEM disassembly (0x0003e778..0x0003e784):
 *
 * Loads the fixed session-handler callback pointer (0x000589cb, Thumb) into the
 * first argument, clears the second to 0, and tail-calls the vendor
 * secure-session bring-up routine (0x00051b0c), returning its int result. The
 * bring-up body itself is vendor and is not reconstructed.
 */
int ble_secure_session_init(void)
{
    return ble_secure_session_start_51b0c((void *)0x000589cbu, 0);
}

/*
 * ble_build_connect_advert_payload — compose and emit the connect advertisement,
 * choosing the bike_id or ecu_serial variant. // 0x0003e948
 *
 * OEM disassembly (0x0003e948..0x0003e988):
 *
 * Clears the 0x3a-byte advert staging region (starting at the pub_key buffer),
 * publishes on the in-process bus topic "vm" with the settings_read_property_by_path
 * handler, capturing its result. When no bike id is programmed
 * (ble_bike_id_present() == 0) it advertises the ecu_serial buffer; otherwise it
 * copies the bike_id buffer into the auth scratch and advertises that. In both
 * cases an id event for the ecu_serial buffer is then queued. Returns the
 * captured bus-publish result.
 */
uint32_t ble_build_connect_advert_payload(void)
{
    uint32_t rc;

    vm_memset_61e62((void *)SETTINGS_PUB_KEY_BUF, 0, 0x3a);
    rc = ble_bus_publish_40618((uint32_t)BLE_BUS_TOPIC_VM,
                               (void *)0x0003e785u /* settings_read_property_by_path | thumb */, 0);

    if (ble_bike_id_present() == 0) {
        ble_advertise_bike_id_payload((const void *)SETTINGS_ECU_SERIAL_BUF, 0xd);
    } else {
        auth_copy_bike_id((const void *)SETTINGS_BIKE_ID_BUF, 0xd);
        ble_advertise_ecu_serial_payload((const void *)SETTINGS_BIKE_ID_BUF, 0xd);
    }
    auth_submit_id_event((const void *)SETTINGS_ECU_SERIAL_BUF, 0xd);
    return rc;
}
