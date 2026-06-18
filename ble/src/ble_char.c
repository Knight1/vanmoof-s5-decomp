/*
 * ble_char.c — VanMoof BLE GATT characteristic value read/write helpers.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   ble_char_write_value_26     @ 0x0003e818  (ecu_serial write, publishes on change)
 *   ble_build_const_response_13 @ 0x0003e860  (copy current ecu_serial value out)
 *   ble_send_32byte_value       @ 0x0003e880  (pub_key write, publishes on change)
 *   ble_char_write_value_13     @ 0x0003e8c8  (bike_id write, publishes/clears)
 *   ble_bike_id_present         @ 0x0003e924  (predicate: a bike id is programmed)
 *
 * The write helpers re-publish on the in-process bus only when the incoming
 * value differs from the stored one (a memcmp short-circuit).
 */

#include "ble.h"

/*
 * ble_char_write_value_26 — GATT characteristic write into the ecu_serial RAM
 * buffer, republishing the value only when it changes. // 0x0003e818
 *
 * OEM disassembly (0x0003e818..0x0003e857):
 *
 * Rejects writes longer than 13 bytes with -EINVAL (-0x16). Otherwise it
 * compares the incoming `len` bytes against the current ecu_serial buffer; when
 * unchanged it returns 0. When the value differs it clears 13 bytes of the
 * buffer, copies the new bytes in (bounded to the 26-byte field capacity), and
 * tail-calls the bus publish helper with the "vm/ecu_serial" topic and a 13-byte
 * payload, returning the publish result.
 */
int ble_char_write_value_26(const void *data, uint32_t len)
{
    if (len >= 0xe) {
        return -0x16;
    }
    if (vm_memcmp_61e00((const void *)SETTINGS_ECU_SERIAL_BUF, data, len) == 0) {
        return 0;
    }
    vm_memset_61e62((void *)SETTINGS_ECU_SERIAL_BUF, 0, 0xd);
    vm_memcpy_bounded_61e3c((void *)SETTINGS_ECU_SERIAL_BUF, data, len, 0x1a);
    return (int)ble_msg_publish_40558(BLE_TOPIC_ECU_SERIAL,
                                      (const void *)SETTINGS_ECU_SERIAL_BUF, 0xd);
}

/*
 * ble_build_const_response_13 — copy the current 13-byte ecu_serial value into
 * the caller-supplied destination buffer. // 0x0003e860
 *
 * OEM disassembly (0x0003e860..0x0003e879):
 *
 * Copies three 32-bit words followed by one trailing byte (13 bytes) from the
 * ecu_serial RAM buffer into the destination via post-incrementing word
 * loads/stores, then returns 13. Used as a fallback payload when an inbound
 * bike_id write is malformed.
 */
uint32_t ble_build_const_response_13(void *dst)
{
    const uint8_t *src = (const uint8_t *)SETTINGS_ECU_SERIAL_BUF;
    uint8_t       *d   = (uint8_t *)dst;
    const uint8_t *end = src + 0xc;

    /* three 32-bit words (unaligned source/dest, Cortex-M4 word access) */
    do {
        *(uint32_t *)d = *(const uint32_t *)src;
        src += 4;
        d   += 4;
    } while (src != end);
    /* trailing byte (13th) */
    *d = *src;
    return 0xd;
}

/*
 * ble_send_32byte_value — copy a 32-byte characteristic value (the public key)
 * into its RAM buffer and republish it on "vm/pub_key" when changed.
 * // 0x0003e880
 *
 * OEM disassembly (0x0003e880..0x0003e8bd):
 *
 * Compares the 32 incoming bytes against the current pub_key value buffer; if
 * identical it returns. Otherwise it zero-fills 32 bytes, copies the 32 new
 * bytes in via post-incrementing word loads/stores, and tail-calls the bus
 * publish helper with the "vm/pub_key" topic and a 32-byte payload.
 */
void ble_send_32byte_value(const void *data)
{
    if (vm_memcmp_61e00((const void *)BLE_PUBKEY_VALUE_BUF, data, 0x20) == 0) {
        return;
    }

    vm_memset_61e62((void *)BLE_PUBKEY_VALUE_BUF, 0, 0x20);

    {
        const uint8_t *src = (const uint8_t *)data;
        uint8_t       *dst = (uint8_t *)BLE_PUBKEY_VALUE_BUF;
        const uint8_t *end = src + 0x20;
        do {
            *(uint32_t *)dst = *(const uint32_t *)src;
            src += 4;
            dst += 4;
        } while (src != end);
    }

    ble_msg_publish_40558(BLE_TOPIC_PUB_KEY, (const void *)BLE_PUBKEY_VALUE_BUF, 0x20);
}

/*
 * ble_char_write_value_13 — GATT characteristic write into the bike_id RAM
 * buffer, publishing vm/bike_id on change. // 0x0003e8c8
 *
 * OEM disassembly (0x0003e8c8..0x0003e91b):
 *
 * Rejects writes longer than 13 bytes with -EINVAL (-0x16). A zero-length write
 * is a clear: it zero-fills 13 bytes, runs the (no-op) bounded copy, and
 * publishes the topic with a NULL payload (the publish-clear thunk), returning
 * that result. For a 1..13 byte write it first compares the new bytes against the
 * current buffer; if unchanged it returns 0, otherwise it zero-fills 13 bytes,
 * copies the new bytes in (bounded to 13), and publishes a 13-byte payload.
 */
int ble_char_write_value_13(const void *data, uint32_t len)
{
    if (len >= 0xe) {
        return -0x16;
    }

    if (len != 0 &&
        vm_memcmp_61e00((const void *)SETTINGS_BIKE_ID_BUF, data, len) == 0) {
        return 0;
    }

    vm_memset_61e62((void *)SETTINGS_BIKE_ID_BUF, 0, 0xd);
    vm_memcpy_bounded_61e3c((void *)SETTINGS_BIKE_ID_BUF, data, len, 0xd);

    if (len == 0) {
        return (int)ble_msg_publish_clear_59bac(BLE_TOPIC_BIKE_ID);
    }

    return (int)ble_msg_publish_40558(BLE_TOPIC_BIKE_ID,
                                      (const void *)SETTINGS_BIKE_ID_BUF, 0xd);
}

/*
 * ble_bike_id_present — report whether the bike_id value buffer holds a non-zero
 * first byte (i.e. whether a bike id has been programmed). // 0x0003e924
 *
 * OEM disassembly (0x0003e924..0x0003e93f):
 *
 * Builds a single 0x00 byte on the stack and compares the first byte of the
 * bike_id buffer against it with the libc memcmp helper, returning 1 when they
 * differ (buffer byte non-zero) and 0 otherwise. The incoming registers are
 * phantom (the prologue only reserves the scratch byte). The caller advertises
 * the bike_id payload only when this returns 0.
 *
 * (Renamed from the earlier guess "ble_record_set_flag_byte": it is a read-only
 * predicate and writes nothing.)
 */
int ble_bike_id_present(void)
{
    uint8_t zero = 0;
    return vm_memcmp_61e00((const void *)SETTINGS_BIKE_ID_BUF, &zero, 1) != 0;
}
