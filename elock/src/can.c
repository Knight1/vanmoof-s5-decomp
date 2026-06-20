/*
 * can.c — CAN Object-Dictionary signalling + diagnostic event records (elock).
 *
 * VanMoof S5/A5 electronic frame-lock controller. The comms-registry primitives
 * (od_registry_lookup / _wait / _release) and the message-buffer send are vendor
 * middleware; the frame assembly, multi-frame chunking, descriptor construction
 * and event-record packing are VanMoof. Translated from the OEM image
 * (LPC546xx, base 0x0). The bus context is accessed at its OEM byte offsets
 * (+0x10 dispatch vtable, +0x594 sub-context, +0x5a4 local send callback).
 */

#include "elock.h"

/*
 * elock_log_event — assemble a 30-byte diagnostic event record and publish it
 * to the lock task's event message buffer.
 *
 * OEM disassembly (0x0000352c..0x00003588):
 *
 * Loads the lock-context pointer-to-pointer from the RAM global at 0x2002d0. If
 * the context has not been installed yet (the inner pointer *ctx is NULL) the
 * call is a no-op. Otherwise it zero-fills a local 30-byte record, then copies
 * argc 32-bit words from the variadic argument list into the record body
 * starting at byte offset 6. The fixed header is then written: the source tag at
 * offset 0 (32-bit) and the event code at offset 4 (16-bit). Finally it
 * publishes the record through the message-buffer send primitive (0x626c),
 * targeting the buffer handle stored at offset 0x590 of *(*ctx), tagging the
 * message with the handler reference at 0x387c and a record length of 0x1e bytes.
 *
 * Same shape as power_control's power_log_event. argc counts 32-bit words of
 * body payload; at most 6 words (24 bytes) fit after the 6-byte header.
 */
void elock_log_event(uint32_t src_tag, uint16_t code, int argc, ...)
{
    /* lock-context pointer-to-pointer (RAM global @0x2002d0) */
    int **ctx = *(int ***)0x2002d0u;
    uint8_t record[30];
    int i;
    /* first variadic word lives just past the named args in the
     * caller's outgoing-argument frame */
    const uint32_t *body = &((const uint32_t *)&argc)[1];

    if (*ctx == 0)
        return;

    mem_set(record, 0, 0x1e);
    for (i = 0; i != argc; i++)
        *(uint32_t *)(record + 6 + i * 4) = body[i];

    *(uint32_t *)(record + 0) = src_tag;
    *(uint16_t *)(record + 4) = code;

    /* publish to the lock task's event message buffer @+0x590 */
    message_buffer_send(*(void **)((char *)*ctx + 0x590),
                        (void *)0x387du, 0, record, 0x1e);
}

/*
 * can_send_multiframe — split a CAN object-dictionary payload into a sequence of
 * <=8-byte CAN frames and dispatch each over the bus.
 *
 * OEM disassembly (0x00005ce6..0x00005d40):
 *
 * Parameters: bus context (r0), the OD record (r1, whose +0x8 holds the total
 * remaining byte count and +0x0 the data pointer, +0x10 the transfer mode), the
 * frame scratch buffer (r2), and a non-zero "send" flag (r3). The `len` argument
 * acts ONLY as a flag: when it is zero the transfer count is zero (nothing is
 * sent); when it is non-zero the real byte count is always taken from the
 * record's stored length at +0x8 (`cbz r3` at 0x5cf0). The loop, while bytes
 * remain:
 *   - chunk = min(remaining, 8); store it as the frame length at frame+0x4.
 *   - if chunk != 0, copy chunk bytes from record_data + offset into the frame
 *     body at frame+0x5.
 *   - re-read the actual stored length from frame+0x4, advance the running
 *     offset by it and decrement remaining by it.
 *   - dispatch the frame through the bus vtable callback at bus+0x10:
 *     (*(*(ctx+0x10)))(ctx, frame).
 *   - increment the sequence counter in the low nibble/3 bits of frame+0x3:
 *     if record+0x10 (mode) == 2 the counter wraps modulo 16, otherwise modulo
 *     8; the high bits of frame+0x3 are preserved.
 */
void can_send_multiframe(void *bus, elock_od_record_t *rec,
                         uint8_t *frame, uint32_t len)
{
    uint32_t offset = 0;
    /* len is a send flag: 0 -> send nothing; non-zero -> use rec->length (+0x8) */
    uint32_t remaining = (len == 0) ? 0 : rec->length;

    while (remaining != 0) {
        uint8_t chunk = (remaining >= 8) ? 8 : (uint8_t)remaining;
        frame[4] = chunk;
        if (remaining != 0 && chunk != 0)
            mem_copy(frame + 5, rec->data + offset, chunk);  /* rec+0x0 */
        chunk = frame[4];
        remaining -= chunk;
        offset += chunk;
        /* dispatch via bus vtable callback @bus+0x10 */
        (*(void (**)(void *, void *))((char *)bus + 0x10))(bus, frame);

        {
            uint8_t seq = frame[3];
            uint8_t next = (uint8_t)(frame[3] + 1);
            if (rec->mode == 2)           /* rec+0x10 */
                seq = (uint8_t)((seq & 0xf0) | (next & 0x0f));
            else
                seq = (uint8_t)((seq & 0xf8) | (next & 0x07));
            frame[3] = seq;
        }
    }
}

/*
 * od_signal_write_u16 — store a 16-bit value into the CAN object-dictionary
 * signal addressed by descriptor {id_lo=0xc1, id_hi=0x03, mode=0} (OD 0x3c1),
 * using the comms-registry lookup / wait / release protocol.
 *
 * OEM disassembly (0x0000620c..0x00006268):
 *
 * Builds a 3-byte OD descriptor on the stack: byte0 = 0xc1, byte1 = 0x03, byte2
 * = 0 (mode), loaded as a single word for the registry calls. Reads the registry
 * handle from arg0+0x0. Looks the signal up; on miss returns. On hit, waits for
 * access (100-tick timeout); a non-zero result also bails. It then writes the
 * caller-supplied 16-bit value into the signal's data buffer (*(entry+0x0)),
 * clears the descriptor mode byte, re-looks-up the same descriptor and releases
 * the entry's +0x4 lock handle.
 */
void od_signal_write_u16(void *ctx, const uint16_t *value)
{
    uint32_t registry = *(uint32_t *)ctx;
    uint8_t desc[4];
    void *entry;

    desc[0] = 0xc1;
    desc[1] = 0x03;
    desc[2] = 0x00;
    desc[3] = 0x00;

    entry = od_registry_lookup(registry, *(uint32_t *)desc);
    if (entry == 0)
        return;
    if (od_registry_wait(*(void **)((char *)entry + 4), 0x64) != 0)
        return;

    /* write the 16-bit signal value into the OD data buffer */
    *(uint16_t *)(*(void **)entry) = *value;

    desc[2] = 0x00;
    desc[0] = 0xc1;
    desc[1] = 0x03;
    entry = od_registry_lookup(registry, *(uint32_t *)desc);
    if (entry == 0)
        return;
    od_registry_release(*(void **)((char *)entry + 4));
}

/*
 * elock_can_send_signal — assemble a 13-byte CAN frame from an OD signal record
 * and dispatch it, either locally (mode 0, via the bus vtable callback) or as a
 * remote multi-frame transfer (mode 1/2, via can_send_multiframe).
 *
 * OEM disassembly (0x00006644..0x000066c8):
 *
 * Validates both pointers; returns -2 if either is NULL. Zero-fills a 13-byte
 * stack frame. Packs the 24-bit CAN identifier from record+0xc into the frame
 * header: the low 16 bits at frame+0x0 and bits 16..23 at frame+0x2. Dispatch is
 * then selected on the record's mode byte at record+0x10:
 *   - mode 1: set the 0x10 flag bit in frame+0x3, then fall into the mode-2 path.
 *   - mode 1/2: remote transfer via can_send_multiframe(bus+0x594, record,
 *     frame, len=1).
 *   - mode 0 (local): if record+0x24 (payload length) is non-zero, copy that
 *     many bytes from record+0x20 into the frame body at frame+0x5 and store the
 *     length byte at frame+0x4. Set the 0x10 flag bit in frame+0x3 and dispatch
 *     the single frame through the local bus vtable callback at bus+0x5a4:
 *     (*(*(bus+0x5a4)))(bus+0x594, frame).
 *   - any other mode: return -1.
 * Returns 0 on the successful paths.
 */
int elock_can_send_signal(void *bus, elock_od_record_t *rec)
{
    uint8_t frame[13];

    if (bus == 0 || rec == 0)
        return -2;

    mem_set(frame, 0, 0xd);
    *(uint16_t *)(frame + 0) = (uint16_t)rec->can_id;        /* rec+0xc */
    frame[2] = (uint8_t)(rec->can_id >> 16);

    if (rec->mode == 1) {                                    /* rec+0x10 */
        frame[3] |= 0x10;
        /* fall through to remote multi-frame */
        can_send_multiframe((char *)bus + 0x594, rec, frame, 1);
        return 0;
    } else if (rec->mode == 2) {
        can_send_multiframe((char *)bus + 0x594, rec, frame, 1);
        return 0;
    } else if (rec->mode == 0) {
        if (rec->payload_len != 0) {
            /* copy the FULL 32-bit payload_len; only the length BYTE is stored in
             * frame[4] (OEM: ldr r2,[rec+0x24] -> mem_copy, strb r2 -> frame+4) */
            mem_copy(frame + 5, rec->payload, rec->payload_len);   /* rec+0x20 */
            frame[4] = (uint8_t)rec->payload_len;
        }
        frame[3] |= 0x10;
        /* dispatch single frame via local bus vtable callback @bus+0x5a4 */
        (*(void (**)(void *, void *))((char *)bus + 0x5a4))(
            (char *)bus + 0x594, frame);
        return 0;
    }
    return -1;
}

/*
 * elock_od_signal_8808_send — read the 8-byte value of the CAN object-dictionary
 * signal addressed by descriptor token {0x08, 0x88, 0} (OD 0x8808), overwrite it
 * with the caller-supplied 8-byte payload, then re-look-up and transmit it.
 *
 * OEM disassembly (0x000066ca..0x0000673c):
 *
 * Builds a 3-byte descriptor {0x08, 0x88, 0}. Looks the signal up; on miss
 * returns -1. Waits for access (timeout 100); on error returns -1. It then
 * copies the caller's two 32-bit words (8 bytes) into the signal's data buffer
 * (*(entry+0x0)) and clears the descriptor mode byte. It re-looks-up the same
 * descriptor; on miss returns -1, otherwise calls elock_can_send_signal to
 * transmit the just-updated signal, saves its return value, and releases the
 * lock handle. Returns the elock_can_send_signal result.
 */
int elock_od_signal_8808_send(void *bus, const uint32_t *value)
{
    uint32_t registry = (uint32_t)(uintptr_t)bus;   /* arg0 used directly as registry handle */
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
    if (od_registry_wait(*(void **)((char *)entry + 4), 0x64) != 0)
        return -1;

    /* copy the 8-byte payload into the OD signal data buffer */
    {
        uint32_t *dst = *(uint32_t **)entry;
        dst[0] = value[0];
        dst[1] = value[1];
    }

    desc[2] = 0x00;
    desc[0] = 0x08;
    desc[1] = 0x88;
    entry = od_registry_lookup(registry, *(uint32_t *)desc);
    if (entry == 0)
        return -1;

    rc = elock_can_send_signal(bus, (elock_od_record_t *)entry);
    od_registry_release(*(void **)((char *)entry + 4));
    return rc;
}
