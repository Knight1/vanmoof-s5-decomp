/*
 * can.c — pedal-sensor CAN-OD event log + secret/key/signal registration.
 *
 * VanMoof S5/A5 pedal-assist / torque + cadence sensor. The message-buffer
 * publisher, the NVM/flash record driver and the OD comms-registry primitives
 * are vendor; the event-record packing and the OD descriptor glue are VanMoof.
 * The OD opcodes 0x87 (secret) / 0x91 (key) / 0x8808 (signal) mirror the elock /
 * light infrastructure. Translated from the OEM image (NXP LPC546xx, base 0x0).
 */

#include "power_pedal.h"

/* power_pedal_log_event — assemble a 30-byte event record and publish it.
 *
 * OEM disassembly (0x00003cb4..0x00003d10):
 *
 * Same shape as elock/eshifter/frontlight/rearlight log_event. Loads the
 * publisher context pointer from the RAM global PP_LOG_CTX; if installed
 * (non-NULL) it zero-fills a 0x1e-byte record, copies argc 32-bit body words into
 * the record at byte offset 6, writes src_tag (u32) at 0 and code (u16) at 4,
 * then publishes via message_buffer_send(*(*ctx+0x590), PP_LOG_TAG, 0, record,
 * 0x1e). The null guard tests the loaded global pointer itself (single deref).
 */
void power_pedal_log_event(uint32_t src_tag, uint16_t code, int argc, ...)
{
    int **ctx = *(int ***)PP_LOG_CTX;   /* publisher context pointer (RAM global) */
    uint8_t record[30];
    int i;
    const uint32_t *body = &((const uint32_t *)&argc)[1];

    if (ctx == 0)
        return;

    mem_set(record, 0, 0x1e);
    for (i = 0; i != argc; i++)
        *(uint32_t *)(record + 6 + i * 4) = body[i];

    *(uint32_t *)(record + 0) = src_tag;
    *(uint16_t *)(record + 4) = code;

    message_buffer_send(*(void **)((char *)*ctx + 0x590),
                        (void *)PP_LOG_TAG, 0, record, 0x1e);
}

/* power_pedal_od_signal_register — read the 14-byte secret from NVM and publish
 * its 13-byte body into OD descriptor 0x87xx.
 *
 * OEM disassembly (0x00006978..0x00006a76):
 *
 * Opens the NVM record reader, reads NVM blob 0x30 (14 bytes); a leading byte > 1
 * marks the record invalid (re-zeroed, status -1). Builds descriptor
 * {selector, 0x87, 0} (selector = desc_id[4], registry = desc_id[0]), OD
 * lookup/wait(100), copies the 13-byte body (staging[1..13]) into the descriptor
 * data slot, re-looks-up and releases. Folds read/publish/close status to a 0/-1
 * return; an optional out buffer is verified via mem_compare. (== rearlight /
 * frontlight_od_signal_register / elock_load_secret_to_od_87. The 0x87 is an OD
 * opcode, not the device node.)
 */
int power_pedal_od_signal_register(const uint8_t *desc_id, void *out_buf)
{
    int rec;
    int read_err;
    int pub_err;
    int ret;
    uint8_t staging[14];
    uint8_t desc[4];
    uint8_t selector;
    uint32_t registry;
    void *node;

    rec = nvm_record_open();
    mem_set(staging, 0, 0xe);
    if (rec == 0) {
        read_err = -1;
    } else {
        read_err = (nvm_record_read(rec, staging, 0x30, 0xe) != 0) ? -1 : 0;
    }
    if (staging[0] > 1) {
        mem_set(staging, 0, 0xe);
        read_err = -1;
    }

    selector = desc_id[4];
    registry = *(const uint32_t *)desc_id;
    desc[0] = selector;
    desc[1] = 0x87;
    desc[2] = 0;
    desc[3] = 0;

    pub_err = -1;
    node = od_registry_lookup(registry, *(uint32_t *)desc);
    if (node != 0 && od_resource_wait(*(void **)((char *)node + 4), 100) == 0) {
        uint8_t *dst = *(uint8_t **)node;
        int i;
        for (i = 0; i < 13; i++)
            dst[i] = staging[1 + i];
        pub_err = 0;
        desc[0] = selector;
        desc[1] = 0x87;
        desc[2] = 0;
        node = od_registry_lookup(registry, *(uint32_t *)desc);
        if (node != 0)
            od_signal_release(*(void **)((char *)node + 4));
        else
            pub_err = -1;
    }
    if (read_err != 0)
        pub_err = -1;

    if (rec == 0) {
        ret = -1;
    } else {
        nvm_record_release(rec);
        ret = 0;
    }
    if (pub_err != 0)
        ret = -1;

    if (out_buf != 0) {
        int cmp = mem_compare(staging, out_buf, 0xe);
        ret = (ret == 0) ? (-(int)(cmp != 0)) : -1;
    }
    return ret;
}

/* power_pedal_od_signal_register_91 — read the 16-byte key from NVM and publish
 * it into OD descriptor 0x91xx.
 *
 * OEM disassembly (0x00006a78..0x00006b60):
 *
 * Same shape as power_pedal_od_signal_register but for the 16-byte key record
 * (NVM blob 0x40, no length-byte sentinel). (== elock_load_aeskey_to_od_91.)
 */
int power_pedal_od_signal_register_91(const uint8_t *desc_id, void *out_buf)
{
    int rec;
    int read_err;
    int pub_err;
    int ret;
    uint8_t staging[16];
    uint8_t desc[4];
    uint8_t selector;
    uint32_t registry;
    void *node;

    rec = nvm_record_open();
    mem_set(staging, 0, 0x10);
    if (rec == 0) {
        read_err = -1;
    } else {
        read_err = (nvm_record_read(rec, staging, 0x40, 0x10) != 0) ? -1 : 0;
    }

    selector = desc_id[4];
    registry = *(const uint32_t *)desc_id;
    desc[0] = selector;
    desc[1] = 0x91;
    desc[2] = 0;
    desc[3] = 0;

    pub_err = -1;
    node = od_registry_lookup(registry, *(uint32_t *)desc);
    if (node != 0 && od_resource_wait(*(void **)((char *)node + 4), 100) == 0) {
        uint8_t *dst = *(uint8_t **)node;
        int i;
        for (i = 0; i < 16; i++)
            dst[i] = staging[i];
        pub_err = 0;
        desc[0] = selector;
        desc[1] = 0x91;
        desc[2] = 0;
        node = od_registry_lookup(registry, *(uint32_t *)desc);
        if (node != 0)
            od_signal_release(*(void **)((char *)node + 4));
        else
            pub_err = -1;
    }
    if (read_err != 0)
        pub_err = -1;

    if (rec == 0) {
        ret = -1;
    } else {
        nvm_record_release(rec);
        ret = 0;
    }
    if (pub_err != 0)
        ret = -1;

    if (out_buf != 0) {
        int cmp = mem_compare(staging, out_buf, 0x10);
        ret = (ret == 0) ? (-(int)(cmp != 0)) : -1;
    }
    return ret;
}

/* power_pedal_od_signal_8808_send — write the 8-byte OD signal 0x8808 and
 * transmit.
 *
 * OEM disassembly (0x000065b8..0x0000662a):
 *
 * Builds descriptor {0x08, 0x88, 0} (OD 0x8808), runs the lookup/wait handshake,
 * copies the two payload words into the descriptor buffer, re-looks-up, dispatches
 * via od_signal_send and releases. Returns the send result, or -1 on any failure.
 * (== elock/rearlight_od_signal_8808_send.)
 */
int power_pedal_od_signal_8808_send(void *bus, const uint32_t *payload)
{
    uint32_t registry = (uint32_t)(uintptr_t)bus;
    uint8_t desc[4];
    void *node;
    int rc;

    desc[0] = 0x08;
    desc[1] = 0x88;
    desc[2] = 0x00;
    desc[3] = 0x00;

    node = od_registry_lookup(registry, *(uint32_t *)desc);
    if (node == 0)
        return -1;
    if (od_resource_wait(*(void **)((char *)node + 4), 100) != 0)
        return -1;

    {
        uint32_t *dst = *(uint32_t **)node;
        dst[0] = payload[0];
        dst[1] = payload[1];
    }

    desc[0] = 0x08;
    desc[1] = 0x88;
    desc[2] = 0x00;
    node = od_registry_lookup(registry, *(uint32_t *)desc);
    if (node == 0)
        return -1;

    rc = od_signal_send(bus, node);
    od_signal_release(*(void **)((char *)node + 4));
    return rc;
}
