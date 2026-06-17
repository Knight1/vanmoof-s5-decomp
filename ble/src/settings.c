/*
 * settings.c — VanMoof BLE application layer.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions:
 *   settings_parse_time_pubkey         @ 0x0003c660
 *   settings_read_property_by_path     @ 0x0003e784
 */

#include "ble.h"

/*
 * settings_parse_time_pubkey — parse the "time" settings record (a CBOR/TLV
 * map carrying "settings_vm_public_" provisioning data) and, on success, set
 * the system clock. // 0x0003c660
 *
 * The OEM-faithful ABI passes the source descriptor through r3 plus one
 * stacked word; both are forwarded verbatim into the reader-init helper.
 *
 * OEM disassembly (0x3c660..0x3c6ea):
 *
 * Flow: build a value reader over the source, parse it as CBOR, require the
 * top item to be a map (major type 0xa0), pull the "time" byte-string field
 * (max 0x15 bytes), scanf it with "%u/%u/%u,%u:%u:%u" into a tm-style block,
 * normalise year (+100) and month (-1), then apply it to the RTC.
 */
void settings_parse_time_pubkey(uint32_t a0, uint32_t a1, uint32_t a2,
                                uint32_t src_a, uint32_t src_b)
{
    /*
     * One r7-relative stack frame (0x78 bytes), accessed at the OEM byte
     * offsets. The CBOR sub-objects and the sscanf output words are all 4-byte
     * aligned (word view); the CBOR type tag at +0x1a is byte-granular.
     *   +0x00  parser scratch        +0x34  value reader (8 words)
     *   +0x0c  CBOR value iterator    +0x1c  extracted "time" byte-string
     *   +0x1a  value-iterator type byte (iterator+0x0e)
     *   +0x54..+0x68  six sscanf outputs {sec,min,hour,mday,mon,year}
     */
    union {
        uint8_t  b[0x78];
        uint32_t w[0x1e];
    } frame;
    int rc;

    (void)a0;
    (void)a1;
    (void)a2;

    settings_value_reader_init(&frame.b[0x34], src_a, src_b);
    rc = cbor_parser_init(&frame.b[0x34], 0, &frame.b[0x00], &frame.b[0x0c]);
    if (rc != 0) {
        return;
    }
    /* require the top item to be a CBOR map (major type 0xa0) */
    if (frame.b[0x1a] != 0xa0) {
        return;
    }
    rc = cbor_map_get_bstr(&frame.b[0x0c], SETTINGS_KEY_TIME, &frame.b[0x1c], 0x15);
    if (rc <= 0) {
        return;
    }

    /* "%u/%u/%u,%u:%u:%u" -> year,month,day,hour,min,sec. The OEM lays these out
     * (high offset to low) so apply_time() receives a {sec,min,hour,mday,mon,
     * year} block based at +0x54. */
    rc = settings_sscanf(&frame.b[0x1c], SETTINGS_TIME_FMT,
                         &frame.w[0x1a],   /* +0x68 year  */
                         &frame.w[0x19],   /* +0x64 month */
                         &frame.w[0x18],   /* +0x60 day   */
                         &frame.w[0x17],   /* +0x5c hour  */
                         &frame.w[0x16],   /* +0x58 min   */
                         &frame.w[0x15]);  /* +0x54 sec   */
    if (rc == 6) {
        frame.w[0x1a] += 100;   /* year:  tm base 1900 -> 2000 */
        frame.w[0x19] -= 1;     /* month: 1-based -> 0-based   */
        settings_apply_time(&frame.w[0x15]);
    }
}

/*
 * settings_read_property_by_path — resolve a "vm/..." settings path against the
 * known leaf properties and emit the matching value through the supplied
 * write callback. // 0x0003e784
 *
 * OEM disassembly (0x3e784..0x3e7fe):
 *
 * Returns 0 on a clean handle (or for a non-leaf / unknown path), otherwise the
 * negative error from the write callback. "pub_key" additionally latches the
 * just-served key into the auth layer via auth_copy_pubkey_32().
 */
int settings_read_property_by_path(const char *path, int key,
                                   ble_settings_write_cb write_cb, void *cb_ctx)
{
    void *seg_rest = (void *)(uintptr_t)key;
    int   token_len;
    int   rc;
    short n;

    token_len = settings_path_token_len(path, &seg_rest);
    if (seg_rest != 0) {
        return 0;
    }

    if (settings_strncmp(path, SETTINGS_KEY_ECU_SERIAL, token_len) != 0) {
        rc = settings_strncmp(path, SETTINGS_KEY_PUB_KEY, token_len);
        if (rc == 0) {
            n = write_cb(cb_ctx, (const void *)SETTINGS_PUB_KEY_BUF, 0x20);
            if (n < 0) {
                return (int)n;
            }
            auth_copy_pubkey_32((const uint32_t *)SETTINGS_PUB_KEY_BUF);
            return 0;
        }
        rc = settings_strncmp(path, SETTINGS_KEY_BIKE_ID, token_len);
        if (rc != 0) {
            return 0;
        }
        n = write_cb(cb_ctx, (const void *)SETTINGS_BIKE_ID_BUF, 0x0d);
        if (n < 0) {
            return (int)n;
        }
        return 0;
    }

    n = write_cb(cb_ctx, (const void *)SETTINGS_ECU_SERIAL_BUF, 0x0d);
    if (n < 0) {
        return (int)n;
    }
    return 0;
}
