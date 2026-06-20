/*
 * can.c — eshifter CAN Object-Dictionary signalling + diagnostic events.
 *
 * VanMoof S5/A5 electronic gear-shifter controller. The comms-registry
 * primitives (od_registry_lookup/_wait/_release), the NVM/OD record I/O and the
 * message-buffer publisher are vendor middleware; the descriptor construction,
 * frame assembly, position-signal encoding and event-record packing are VanMoof.
 * Translated from the OEM image (NXP LPC546xx, base 0x0). The OD context is
 * accessed at its OEM byte offsets (+0x594 sub-context, +0x5a4 local send cb).
 */

#include "eshifter.h"

/* eshifter_log_event — assemble a 30-byte event record and publish it.
 *
 * OEM disassembly (0x0000332c..0x00003388):
 *
 * Same shape as elock_log_event / power_log_event. Loads the publisher context
 * pointer-to-pointer from the RAM global at 0x2000077c; if *ctx is installed it
 * zero-fills a 0x1e-byte record, copies argc 32-bit body words from the variadic
 * tail into the record at byte offset 6, writes src_tag (u32) at 0 and code
 * (u16) at 4, then publishes via message_buffer_send(*(*ctx+0x590), 0x387d, 0,
 * record, 0x1e).
 */
void eshifter_log_event(uint32_t src_tag, uint16_t code, int argc, ...)
{
    int **ctx = *(int ***)0x2000077cu;   /* publisher context pointer (RAM global) */
    uint8_t record[30];
    int i;
    /* first variadic word lives just past the named args */
    const uint32_t *body = &((const uint32_t *)&argc)[1];

    if (ctx == 0)   /* OEM guards the global pointer itself (single deref) */
        return;

    mem_set(record, 0, 0x1e);
    for (i = 0; i != argc; i++)
        *(uint32_t *)(record + 6 + i * 4) = body[i];

    *(uint32_t *)(record + 0) = src_tag;
    *(uint16_t *)(record + 4) = code;

    message_buffer_send(*(void **)((char *)*ctx + 0x590),
                        (void *)0x387du, 0, record, 0x1e);
}

/* eshifter_od_signal_send_51 — OD block-write of descriptor 0x51 (position /
 * telemetry signal) with a 0xff-fill fallback.
 *
 * OEM disassembly (0x00003988..0x000039e4):
 *
 * Calls nvm_record_block_write(dev, 0x51, offset, buf, len, offset). On success
 * (0) it logs eshifter_log_event(0x3a36c02a, 0xa7, 3, {offset, buf[0],
 * buf[len/2-1]}) (the OEM computes the last-sample index as (len>>1)+0x7fffffff
 * == len/2-1). On failure it re-fills buf with 0xff and re-issues the write.
 * Returns non-zero (the write result) as the bool-ish status.
 */
int eshifter_od_signal_send_51(void *dev, uint32_t offset, uint16_t *buf, uint32_t len)
{
    int rc;

    rc = nvm_record_block_write(dev, 0x51, offset, buf, len, offset);
    if (rc == 0) {
        eshifter_log_event(0x3a36c02au, 0xa7, 3,
                           offset,
                           (uint32_t)buf[0],
                           (uint32_t)buf[(len >> 1) + 0x7fffffff]);
    } else {
        mem_set(buf, 0xff, len);
        nvm_record_block_write(dev, 0x51, offset, buf, len, offset);
    }
    return rc != 0;
}

/* eshifter_read_position_status — read OD descriptor 0x51 and parse the
 * position-sensor sample array.
 *
 * OEM disassembly (0x000039ec..0x00003a9e):
 *
 * Clears ctx->sel_index (+0x8c), seeds ctx->position (+0x8a) to the 0x8000
 * sentinel, and reads OD descriptor 0x51 into a 0xfe-byte frame buffer
 * (od_descriptor_read). On read error returns the sentinel. Otherwise sets
 * ctx->valid (+8). If frame.magic == the 0x3a36c02a 'VMFW' tag:
 *   - type 2: scan up to 124 samples for the first != 0xffff; store
 *     (sample-0x8000) into ctx->position and ((index&0x7f)<<1) into sel_index.
 *   - type 1: log code 0x146 (subcode), then emit a VMFW header.
 *   - other:  log code 0x14a (type), set ctx->err (+9).
 * If magic mismatched: log code 0x14f, then emit a VMFW header.
 * Returns ctx->position (s16).
 */
int eshifter_read_position_status(uint32_t *ctx)
{
    uint8_t  status_byte = 0;
    uint8_t  recbuf[0x100];
    eshifter_pos_frame_t *fr = (eshifter_pos_frame_t *)recbuf;
    int rc;

    *(volatile uint8_t  *)((char *)ctx + 0x8c) = 0;        /* sel_index = 0 */
    *(volatile uint16_t *)((char *)ctx + 0x8a) = 0x8000;   /* position = sentinel */

    rc = od_descriptor_read((void *)ctx[0], 0x51, &status_byte, recbuf, 0xfe);
    if (rc != 0)
        return *(int16_t *)((char *)ctx + 0x8a);
    *(volatile uint8_t *)((char *)ctx + 8) = 1;            /* valid = 1 */

    if (fr->magic == 0x57464d56u) {   /* 'VMFW' magic (literal @0x3aa0) */
        if (fr->frame_type == 2) {
            unsigned int i;
            *(volatile uint16_t *)((char *)ctx + 0x8a) = 0x8000;
            for (i = 0; i != 0x7c; i++) {
                if (fr->samples[i] != (int16_t)0xffff) {
                    *(volatile int16_t *)((char *)ctx + 0x8a) =
                        (int16_t)(fr->samples[i] - 0x8000);
                    *(volatile uint8_t *)((char *)ctx + 0x8c) =
                        (uint8_t)((i & 0x7f) << 1);
                    return *(int16_t *)((char *)ctx + 0x8a);
                }
            }
            *(volatile uint8_t *)((char *)ctx + 0x8c) = 0;
            return *(int16_t *)((char *)ctx + 0x8a);
        }
        if (fr->frame_type != 1) {
            eshifter_log_event(0x3a36c02au, 0x14a, 1, (uint32_t)fr->frame_type);
            *(volatile uint8_t *)((char *)ctx + 9) = 1;    /* err = 1 */
            return *(int16_t *)((char *)ctx + 0x8a);
        }
        eshifter_log_event(0x3a36c02au, 0x146, 1, (uint32_t)fr->subcode);
    } else {
        eshifter_log_event(0x3a36c02au, 0x14f, 0);
    }
    eshifter_emit_vmfw_header(ctx, recbuf);
    return *(int16_t *)((char *)ctx + 0x8a);
}

/* eshifter_send_position_signal — deadbanded position/telemetry transmit.
 *
 * OEM disassembly (0x0000700e..0x00007094):
 *
 * Acquires the OD registry lock (od_registry_wait(*(ctx+4), INT_MAX)). If
 * ctx->err (+9) is clear and the new position is outside the +-3 deadband
 * around ctx->position (+0x8a), it clears ctx->valid (+8) and stages a 4-byte OD
 * frame {0xffff marker, position}. If the next slot would overflow the page
 * ((sel_index+10) > 0xff) it flush-sends 2 bytes and resets sel_index; it then
 * always sends 4 bytes via eshifter_od_signal_send_51 at offset (sel_index+6).
 * If ctx->valid is now set it returns ctx->position, else refreshes via
 * eshifter_read_position_status. Releases the lock.
 */
int eshifter_send_position_signal(int ctx, int position)
{
    int rc;
    uint16_t frame[2];   /* [0]=0xffff marker, [1]=position */

    od_registry_wait(*(void **)(ctx + 4), 0x7fffffff);

    if (*(char *)(ctx + 9) == 0 &&
        (position < *(int16_t *)(ctx + 0x8a) - 3 ||
         *(int16_t *)(ctx + 0x8a) + 3 < position)) {
        *(volatile uint8_t *)(ctx + 8) = 0;          /* valid = 0 */
        frame[0] = 0xffff;
        frame[1] = (uint16_t)position;

        if ((uint32_t)(*(uint8_t *)(ctx + 0x8c) + 10) > 0xff) {
            eshifter_od_signal_send_51((void *)ctx,
                (uint32_t)((*(uint8_t *)(ctx + 0x8c) + 6) & 0xff), frame, 2);
            *(volatile uint8_t *)(ctx + 0x8c) = 0;   /* sel_index = 0 */
        }
        eshifter_od_signal_send_51((void *)ctx,
            (uint32_t)((*(uint8_t *)(ctx + 0x8c) + 6) & 0xff), frame, 4);
    }

    if (*(int16_t *)(ctx + 8) == 0)
        rc = eshifter_read_position_status((uint32_t *)ctx);
    else
        rc = *(int16_t *)(ctx + 0x8a);
    od_registry_release(*(void **)(ctx + 4));
    return rc;
}

/* eshifter_encode_status_frame — build a 13-byte status record from an OD
 * descriptor and dispatch (local vtable or multi-frame).
 *
 * OEM disassembly (0x00006a74..0x00006af8):
 *
 * Returns -2 if ctx/desc NULL. Zero-fills a 13-byte record, packs the 24-bit
 * CAN id (desc+0xc) into record[0..2]. By desc->mode (desc+0x10): mode 1 sets
 * the 0x10 flag in record[3] and falls into the mode-2 multi-frame send; mode 2
 * sends via can_send_multiframe(ctx+0x594, desc, record, 1); mode 0 copies the
 * payload (desc+0x20, length desc+0x24) into record[5..], stores the length in
 * record[4], sets the 0x10 flag and dispatches via the local OD send vtable at
 * ctx+0x5a4; any other mode returns -1.
 */
int eshifter_encode_status_frame(void *ctx, const od_msg_desc_t *desc)
{
    uint8_t rec[13];
    char mode;

    if (ctx == 0 || desc == 0)
        return -2;

    mem_set(rec, 0, 0xd);
    rec[0] = (uint8_t)desc->can_id;
    rec[1] = (uint8_t)(desc->can_id >> 8);
    rec[2] = (uint8_t)(desc->can_id >> 0x10);

    mode = (char)desc->mode;
    if (mode == 1) {
        rec[3] |= 0x10;
        can_send_multiframe((char *)ctx + 0x594, desc, rec, 1);
        return 0;
    } else if (mode == 2) {
        can_send_multiframe((char *)ctx + 0x594, desc, rec, 1);
        return 0;
    } else if (mode == 0) {
        if (desc->payload_len != 0) {
            rec[4] = (uint8_t)desc->payload_len;
            mem_copy(&rec[5], desc->payload, desc->payload_len);
        }
        rec[3] |= 0x10;
        (*(od_send_fn *)((char *)ctx + 0x5a4))((char *)ctx + 0x594, rec);
        return 0;
    }
    return -1;
}

/* eshifter_od_signal_8808_send — write the 8-byte OD signal 0x8808 and transmit.
 *
 * OEM disassembly (0x00006afa..0x00006b6c):
 *
 * Builds descriptor {0x08, 0x88, 0} (OD 0x8808), runs the lookup/wait handshake,
 * copies the two payload words into the acquired buffer, re-looks-up the
 * descriptor, encodes/sends the status frame and releases the lock. Returns the
 * encode result, or -1 on any lookup/wait failure. (Same shape as
 * elock_od_signal_8808_send.)
 */
int eshifter_od_signal_8808_send(void *ctx, const uint32_t payload[2])
{
    uint32_t registry = (uint32_t)(uintptr_t)ctx;
    uint8_t desc[4];
    void *entry;
    int rc;

    desc[0] = 0x08;
    desc[1] = 0x88;
    desc[2] = 0x00;
    desc[3] = 0x00;

    entry = od_registry_lookup(registry, *(uint32_t *)desc);
    if (entry == 0)
        return -1;
    if (od_registry_wait(*(void **)((char *)entry + 4), 100) != 0)
        return -1;

    {
        uint32_t *dst = *(uint32_t **)entry;
        dst[0] = payload[0];
        dst[1] = payload[1];
    }

    desc[2] = 0x00;
    desc[0] = 0x08;
    desc[1] = 0x88;
    entry = od_registry_lookup(registry, *(uint32_t *)desc);
    if (entry == 0)
        return -1;

    rc = eshifter_encode_status_frame(ctx, (const od_msg_desc_t *)entry);
    od_registry_release(*(void **)((char *)entry + 4));
    return rc;
}

/* eshifter_od_id_exchange — read the 14-byte device id from NVM and publish its
 * 13-byte body into OD descriptor 0x87xx.
 *
 * OEM disassembly (0x00006ca4..0x00006d74):
 *
 * Opens an NVM record proxy, reads record 0x30 (14 bytes); a leading status byte
 * > 1 invalidates the record. Builds descriptor {node, 0x87, 0} (node = arg[4]),
 * does lookup/wait, copies the 13-byte id body (idbuf[1..13]) into the OD buffer,
 * re-looks-up and releases. Folds read / publish / close status to a 0/-1 return;
 * an optional out pointer is verified against the read-back via mem_compare.
 */
int eshifter_od_id_exchange(const uint32_t *arg, void *out_id14)
{
    uint8_t idbuf[14];
    void *nvm;
    int read_rc;
    int send_rc;
    int *slot;
    int rc;
    int result;

    nvm = nvm_record_alloc();
    mem_set(idbuf, 0, 0xe);
    if (nvm == 0) {
        read_rc = -1;
    } else {
        read_rc = (nvm_record_read(nvm, idbuf, 0x30, 0xe) != 0) ? -1 : 0;
    }
    if (idbuf[0] > 1) {
        mem_set(idbuf, 0, 0xe);
        read_rc = -1;
    }

    {
        uint32_t reg  = arg[0];
        uint8_t  node = ((const uint8_t *)arg)[4];
        uint8_t  desc[4];
        desc[0] = node;
        desc[1] = 0x87;
        desc[2] = 0x00;
        desc[3] = 0x00;

        send_rc = -1;
        slot = (int *)od_registry_lookup(reg, *(uint32_t *)desc);
        if (slot != 0 && od_registry_wait((void *)slot[1], 100) == 0) {
            uint8_t *buf = (uint8_t *)slot[0];
            mem_copy(buf, &idbuf[1], 0xd);          /* 13-byte id body */
            rc = (int)(uintptr_t)od_registry_lookup(reg, (uint16_t)(0x8700 | node));
            if (rc != 0) {
                od_registry_release(*(void **)(uintptr_t)(rc + 4));
                send_rc = 0;
            }
        }
    }
    if (read_rc != 0)
        send_rc = -1;

    if (nvm == 0) {
        result = -1;
    } else {
        nvm_record_free(nvm);
        result = 0;
    }
    if (send_rc != 0)
        result = -1;

    if (out_id14 != 0) {
        int cmp = mem_compare(idbuf, out_id14, 0xe);
        result = (result == 0) ? ((cmp != 0) ? -1 : 0) : -1;
    }
    return result;
}

/* eshifter_od_send_signal_13b — persist a 14-byte OD record (status byte 1 +
 * 13-byte payload), commit, then read back via the id exchange.
 *
 * OEM disassembly (0x00006f0c..0x00006f6a):
 *
 * Opens an NVM proxy; stages rec[0]=1 + 13 payload bytes; sends via the
 * multi-frame seam (can_send_multiframe_ctx), commits (nvm_record_commit flag 1)
 * and frees the proxy; then runs eshifter_od_id_exchange(arg, rec) to confirm.
 * Returns the exchange result on success, else -1. The second argument is stored
 * but unused (dead parameter).
 */
int eshifter_od_send_signal_13b(const uint32_t *arg, uint32_t unused, const void *payload13)
{
    uint8_t rec[20];
    void *nvm;
    int tx_rc;
    int commit;
    int status;
    int ret;

    (void)unused;

    nvm = nvm_record_alloc();
    if (nvm == 0)
        return -1;

    rec[0] = 1;
    mem_copy(&rec[1], payload13, 0xd);
    tx_rc  = can_send_multiframe_ctx(nvm, rec, 0xe, 0);
    commit = nvm_record_commit(nvm, 1);
    status = (tx_rc == 0) ? ((commit != 0) ? -1 : 0) : -1;
    nvm_record_free(nvm);

    ret = eshifter_od_id_exchange(arg, rec);
    if (status == 0)
        return ret;
    return -1;
}
