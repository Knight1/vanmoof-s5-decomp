/*
 * ble_message.c — VanMoof BLE message-layer handlers.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   ble_ftp_command_handler     @ 0x0003e9a0  ("ftp_command" firmware/flash transfer)
 *   ble_message_dispatch_by_id  @ 0x0003edbc  (parse request, dispatch by command id)
 */

#include "ble.h"

/*
 * ble_ftp_command_handler — the BLE "ftp_command" handler: drives a chunked
 * firmware / flash-blob transfer over the link. // 0x0003e9a0
 *
 * OEM disassembly (0x0003e9a0..0x0003ed98):
 *
 * A JSON/CBOR reply writer is initialised over the static reply buffer
 * (FTP_REPLY_BUF, cap 0x40) and a CBOR parser over the incoming value-reader
 * (built from src_a/src_b). If the outer reply object cannot be opened the
 * handler returns. The request must parse as a CBOR map (root major type 0xa0);
 * it reads the integer "cmd" field (parse failure -> status 6). The optional
 * "silent" field, when present and truthy, suppresses the reply transmission.
 * `cmd` (0..4) selects a branch (anything else -> status 7):
 *
 *  cmd 0 (open): status 2 if a transfer is already active. Reads {size, name}.
 *   The name is copied to FTP_NAME_BUF and matched against "app_update"
 *   (region 0) then "fmna_blob" (region 1); no match -> status 4. The selected
 *   24-byte descriptor becomes the active region. If its size (+0x10) < the
 *   requested size, the region is cleared and status 3. Otherwise a
 *   "chunk_size":512 field is emitted and, if the descriptor flag (+0x00) is
 *   non-zero, the whole region is erased up front. Status 0.
 *
 *  cmd 1 (close/abort): if a region is active and has a completion callback
 *   (+0x14) it is invoked with code 2; the region is cleared. Status 0.
 *
 *  cmd 2 (write): requires an active region. Reads {index, data}. The data is
 *   copied to FTP_DATA_BUF (default 512). If index >= size/512 -> status 5. The
 *   flash offset is descriptor.offset + index*512; for an erase-on-write region
 *   (flag 0) and an 8-block-aligned index a 0x1000 page is erased first; the
 *   data (length padded up to a multiple of 4) is then written. Status 0.
 *
 *  cmd 3 (verify/finalise): reads optional {size} and {crc}. When size was
 *   supplied, a CRC-16 over [offset, size) is compared to crc: mismatch invokes
 *   the callback with code 1, clears the region, status 1. On match (or when
 *   size was absent) the callback is invoked with code 0, status 0. (As in the
 *   OEM, when "size" is absent the "crc" cursor is read while uninitialised.)
 *
 *  cmd 4 (read): requires an active region (else status 9). Reads {index} and
 *   optional {n} (block count, default 1). If index+n > size/512 -> status 8.
 *   Otherwise a CRC-16 over n*512 bytes at offset+index*512 is returned in the
 *   reply "crc" field; status 0.
 *
 * On exit the reply emits "cmd" (echo) and "status", the object is closed, and
 * unless the silent flag was set the reply is transmitted as a 0xb1 message on
 * `conn` from FTP_REPLY_BUF.
 */
void ble_ftp_command_handler(uint32_t a0, uint32_t a1, uint32_t conn,
                             uint32_t src_a, uint32_t src_b)
{
    /* JSON/CBOR reply writer state */
    uint8_t  json_hdr[16];     /* writer header over FTP_REPLY_BUF */
    void    *json_ctx[4];      /* writer/begin context */
    uint8_t  json_state[16];   /* open/close state */

    /* incoming CBOR parser state */
    uint8_t  value_reader[36]; /* settings value reader over src_a/src_b */
    uint8_t  parser[12];       /* CBOR parser scratch */
    uint8_t  root[16];         /* root value cursor; root[0xe] = major type */
    uint8_t  field0[16];       /* per-field value cursors; [0xe] = type byte */
    uint8_t  field1[16];

    uint32_t cmd = 0;
    uint32_t size_val;
    uint32_t count_val;
    uint32_t idx_val;
    int      status;
    int      silent;
    int      r;
    int      idx;

    const ftp_blob_desc_t        *desc;
    const struct ble_flash_device *flash = BLE_FLASH_DEV;

    (void)a0;
    (void)a1;

    ble_json_writer_init(json_hdr, (void *)FTP_REPLY_BUF, 0x40);
    ble_json_begin(json_ctx, json_hdr, 0);
    settings_value_reader_init(value_reader, src_a, src_b);
    if (ble_json_open(json_ctx, json_state, 0xffffffffu) != 0) {
        return;
    }

    if ((cbor_parser_init(value_reader, 0, parser, root) != 0) ||
        (root[0xe] != 0xa0) ||
        (cbor_map_find_6042e(root, FTP_KEY_CMD, field0) != 0) ||
        (cbor_read_int_58a12(field0, &cmd) != 0)) {
        silent = 0;
        status = 6;
        goto reply;
    }

    /* "silent" suppresses the reply only when present and truthy */
    r = cbor_map_find_6042e(root, FTP_KEY_SILENT, field1);
    silent = (r != 0) && (*(short *)(field1 + 0x0c) != 0);

    status = (int)cmd;
    switch (cmd) {
    case 0: /* open named region */
        if (FTP_ACTIVE_DESC != 0) {
            status = 2;
            goto reply;
        }
        if ((cbor_map_find_6042e(root, FTP_KEY_SIZE, field0) == 0) &&
            (field0[0x0e] == 0) &&
            (cbor_read_int_58a12(field0, &size_val) == 0) &&
            (cbor_map_find_6042e(root, FTP_KEY_NAME, field1) == 0) &&
            (field1[0x0e] == 0x60) &&
            (cbor_get_bstr_len_6041e(field1, &idx_val) == 0) &&
            (idx_val < 0x21) &&
            (cbor_copy_bstr_4c1bc(field1, (void *)FTP_NAME_BUF, &idx_val, 0) == 0)) {
            uint32_t n;

            n = vm_strlen_36d1c(FTP_STR_APP_UPDATE);
            if (n > 0x1f) {
                n = 0x20;
            }
            ble_log_58f0a(FTP_STR_COMPARING, (void *)FTP_NAME_BUF, FTP_STR_APP_UPDATE, n);
            idx = 0;
            if (settings_strncmp((const char *)FTP_NAME_BUF, FTP_STR_APP_UPDATE, (int)n) != 0) {
                n = vm_strlen_36d1c(FTP_STR_FMNA_BLOB);
                if (n > 0x1f) {
                    n = 0x20;
                }
                ble_log_58f0a(FTP_STR_COMPARING, (void *)FTP_NAME_BUF, FTP_STR_FMNA_BLOB, n);
                if (settings_strncmp((const char *)FTP_NAME_BUF, FTP_STR_FMNA_BLOB, (int)n) != 0) {
                    FTP_ACTIVE_DESC = 0;
                    status = 4;
                    goto reply;
                }
                idx = 1;
            }
            desc = (const ftp_blob_desc_t *)(FTP_BLOB_TABLE + (uint32_t)idx * 0x18u);
            FTP_ACTIVE_DESC = (uint32_t)desc;
            if (desc->size < size_val) {
                FTP_ACTIVE_DESC = 0;
                status = 3;
            } else {
                ble_json_emit_key_58a36(json_ctx, FTP_KEY_CHUNK_SIZE);
                ble_json_add_field_5fe92(json_ctx, 0, 0x200, 0);
                desc = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
                if (desc->flag != 0) {
                    flash->api->erase(flash, desc->offset, desc->size);
                }
            }
            goto reply;
        }
        break;

    case 1: /* close / abort */
        if (FTP_ACTIVE_DESC != 0) {
            const ftp_blob_desc_t *d = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
            if (d->callback != 0) {
                ((void (*)(int))d->callback)(2);
            }
        }
        status = 0;
        FTP_ACTIVE_DESC = 0;
        goto reply;

    case 2: /* write data chunk */
        if (FTP_ACTIVE_DESC == 0) {
            goto reply_default;
        }
        count_val = 0x200;
        if ((cbor_map_find_6042e(root, FTP_KEY_INDEX, field0) == 0) &&
            (field0[0x0e] == 0) &&
            (cbor_read_int_58a12(field0, &idx_val) == 0) &&
            (cbor_map_find_6042e(root, FTP_KEY_DATA, field1) == 0) &&
            (field1[0x0e] == 0x40) &&
            (cbor_copy_bstr_4c1bc(field1, (void *)FTP_DATA_BUF, &count_val, 0) == 0)) {
            uint32_t flash_off;
            uint32_t pad;

            ble_json_emit_key_58a36(json_ctx, FTP_KEY_INDEX);
            ble_json_add_field_5fe92(json_ctx, 0, idx_val,
                                     (uint32_t)((int32_t)idx_val >> 31));
            desc = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
            if ((desc->size >> 9) <= idx_val) {
                status = 5;
                goto reply;
            }
            flash_off = desc->offset + idx_val * 0x200u;
            if ((desc->flag == 0) && ((idx_val & 7) == 0)) {
                flash->api->erase(flash, flash_off, 0x1000);
            }
            pad = 0;
            if ((count_val & 3) != 0) {
                pad = 4 - (count_val & 3);
            }
            flash->api->write(flash, flash_off, (void *)FTP_DATA_BUF, pad + count_val);
            status = 0;
            goto reply;
        }
        break;

    case 3: { /* verify CRC / finalise */
        const ftp_blob_desc_t *d;
        int have_size;

        idx_val = 0; /* size accumulator */
        have_size = cbor_map_find_6042e(root, FTP_KEY_SIZE, field0);
        if (((have_size != 0) ||
             ((cbor_map_find_6042e(root, FTP_KEY_CRC, field1) == 0) &&
              (cbor_read_int_58a12(field0, &idx_val) == 0))) &&
            (cbor_read_int_58a12(field1, &count_val) == 0)) {

            if ((have_size == 0) &&
                (ble_crc16_58d72((const void *)((const ftp_blob_desc_t *)FTP_ACTIVE_DESC)->offset,
                                 (int)idx_val) != count_val)) {
                d = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
                if ((d != 0) && (d->callback != 0)) {
                    ((void (*)(int))d->callback)(1);
                }
                FTP_ACTIVE_DESC = 0;
                status = 1;
                goto reply;
            }
            d = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
            status = 0;
            if (d == 0) {
                goto reply;
            }
            if (d->callback == 0) {
                goto reply;
            }
            ((void (*)(int))d->callback)(0);
            status = 0;
            goto reply;
        }
        FTP_ACTIVE_DESC = 0;
        break;
    }

    case 4: /* read block(s) -> return CRC */
        if (FTP_ACTIVE_DESC == 0) {
            status = 9;
            goto reply;
        }
        if ((cbor_map_find_6042e(root, FTP_KEY_INDEX, field0) == 0) &&
            (field0[0x0e] == 0)) {
            cbor_read_int_58a12(field0, &idx_val);
            count_val = 1;
            if ((cbor_map_find_6042e(root, FTP_KEY_N, field1) == 0) &&
                (field1[0x0e] == 0)) {
                cbor_read_int_58a12(field1, &count_val);
            }
            desc = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
            if (idx_val + count_val <= (desc->size >> 9)) {
                uint32_t crc;
                desc = (const ftp_blob_desc_t *)FTP_ACTIVE_DESC;
                crc = ble_crc16_58d72((const void *)(desc->offset + idx_val * 0x200u),
                                      (int)(count_val << 9));
                ble_json_emit_key_58a36(json_ctx, FTP_KEY_CRC);
                ble_json_add_field_5fe92(json_ctx, 0, crc, 0);
                status = 0;
                goto reply;
            }
        }
        break;

    default:
    reply_default:
        status = 7;
        goto reply;
    }

    status = 8;

reply:
    ble_json_emit_key_58a36(json_ctx, FTP_KEY_CMD);
    ble_json_add_field_5fe92(json_ctx, 0, cmd, (uint32_t)((int32_t)cmd >> 31));
    ble_json_emit_key_58a36(json_ctx, FTP_KEY_STATUS);
    ble_json_add_field_5fe92(json_ctx, 0, (uint32_t)status,
                             (uint32_t)((int32_t)status >> 31));
    if ((ble_json_close(json_ctx, json_state) == 0) && !silent) {
        ble_msg_transmit(conn, 0xb1, (void *)FTP_REPLY_BUF,
                         *(uint32_t *)((uint8_t *)json_ctx[0] + 4), 0, 1);
    }
}

/*
 * ble_message_dispatch_by_id — parse an incoming CBOR request, pull out a 16-bit
 * command id (and an optional "subscribe" flag), then dispatch to the matching
 * handler in the BLE command table. // 0x0003edbc
 *
 * OEM disassembly (0x0003edbc..0x0003ee53):
 *
 * The caller hands in a source byte buffer (src/len) plus a context word that is
 * forwarded verbatim to whichever handler fires. A CBOR value reader is started
 * over the source; if the parse fails or the root is not a CBOR map (type 0xa0)
 * the routine returns. The command id lives under map key "tpc": read first as
 * an unsigned integer, falling back to a CBOR double converted via the toolchain
 * d2iz helper (a failed double read aborts). The id is narrowed to 16 bits.
 * The boolean key "sub" is then read into a pre-zeroed flag; when set, the
 * routine walks the broadcast table and emits an announce message for every
 * entry id. Finally it always walks the dispatch table and, for each entry whose
 * id equals the requested id, invokes handler(arg, id, ctx) (no break on match).
 */
void ble_message_dispatch_by_id(uint32_t a0, uint32_t a1, uint32_t ctx,
                                uint32_t src, uint32_t len)
{
    uint8_t  reader[36];     /* CBOR value reader */
    uint8_t  parse_out[12];  /* CBOR parser scratch */
    uint8_t  value_ctx[16];  /* CBOR value context; [0xe] = root type byte */
    char     root_type;
    uint32_t cmd_id;
    uint32_t sub_flag;
    uint32_t dval[2];        /* CBOR double, as (lo, hi) words */
    const ble_cmd_entry_t *e;

    (void)a0;
    (void)a1;

    settings_value_reader_init(reader, src, len);
    if (cbor_parser_init(reader, 0, parse_out, value_ctx) != 0) {
        return;
    }

    root_type = *(volatile char *)((uintptr_t)value_ctx + 0xe);
    if (root_type != (char)0xa0) {
        return;
    }

    /* command id under key "tpc": integer, falling back to double */
    if (cbor_map_get_u32(value_ctx, BLE_CMD_KEY_TPC, &cmd_id) != 0) {
        if (cbor_map_get_f64(value_ctx, BLE_CMD_KEY_TPC, dval) != 0) {
            return;
        }
        cmd_id = (uint32_t)aeabi_d2iz(dval[0], dval[1]);
    }

    {
        uint16_t id = (uint16_t)cmd_id;

        /* boolean "sub" flag: pre-zero the low byte before the read */
        sub_flag = 0;
        cbor_map_get_bool(value_ctx, BLE_CMD_KEY_SUB, &sub_flag);

        if ((sub_flag & 0xff) != 0) {
            for (e = (const ble_cmd_entry_t *)BLE_CMD_BROADCAST_TABLE;
                 e < (const ble_cmd_entry_t *)BLE_CMD_BROADCAST_TABLE_END;
                 e++) {
                ble_announce_command_id(e->id);
            }
        }

        for (e = (const ble_cmd_entry_t *)BLE_CMD_DISPATCH_TABLE;
             e < (const ble_cmd_entry_t *)BLE_CMD_DISPATCH_TABLE_END;
             e++) {
            if (e->id == id) {
                ((ble_cmd_handler_fn)e->handler)(e->arg, id, ctx);
            }
        }
    }
}
