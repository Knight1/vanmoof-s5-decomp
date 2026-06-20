/*
 * can.c — front LED CAN-OD event log + signal registration.
 *
 * VanMoof S5/A5 front LED light controller. The message-buffer publisher, the
 * NVM/flash record driver and the OD comms-registry primitives are vendor; the
 * event-record packing and the node-0x87 descriptor glue are VanMoof. Translated
 * from the OEM image (NXP LPC546xx, base 0x0).
 */

#include "frontlight.h"

/* frontlight_log_event — assemble a 30-byte event record and publish it.
 *
 * OEM disassembly (0x0000313c..0x00003198):
 *
 * Same shape as elock/eshifter log_event. Loads the publisher context pointer
 * from the RAM global FL_LOG_CTX; if it is installed (non-NULL) it zero-fills a
 * 0x1e-byte record, copies argc 32-bit body words from the variadic tail into
 * the record at byte offset 6, writes src_tag (u32) at 0 and code (u16) at 4,
 * then publishes via message_buffer_send(*(*ctx+0x590), FL_LOG_TAG, 0, record,
 * 0x1e). The null-guard tests the loaded global pointer itself (single deref).
 */
void frontlight_log_event(uint32_t src_tag, uint16_t code, int argc, ...)
{
    int **ctx = *(int ***)FL_LOG_CTX;   /* publisher context pointer (RAM global) */
    uint8_t record[30];
    int i;
    /* first variadic word lives just past the named args */
    const uint32_t *body = &((const uint32_t *)&argc)[1];

    if (ctx == 0)
        return;

    mem_set(record, 0, 0x1e);
    for (i = 0; i != argc; i++)
        *(uint32_t *)(record + 6 + i * 4) = body[i];

    *(uint32_t *)(record + 0) = src_tag;
    *(uint16_t *)(record + 4) = code;

    message_buffer_send(*(void **)((char *)*ctx + 0x590),
                        (void *)FL_LOG_TAG, 0, record, 0x1e);
}

/* frontlight_od_signal_register — read the 14-byte device record from NVM and
 * publish its body into the comms Object-Dictionary descriptor 0x87xx.
 *
 * OEM disassembly (0x00005b3e..0x00005c3c):
 *
 * Called from main_init. spec[0] is the OD bus/registry handle; the low byte of
 * spec[1] is the signal sub-index selector. Opens the NVM record reader and
 * reads record 0x30 (14 bytes) into a scratch buffer; a leading byte > 1 marks
 * the record invalid (re-zeroed, status -1). Builds the descriptor {selector,
 * 0x87, 0} (node 0x87 = frontlight), does the OD lookup/wait handshake, copies
 * the 13-byte device body (devbuf[1..13]) into the descriptor data slot,
 * re-looks-up and publishes (od_signal_send), and releases the reader. Folds the
 * read / publish / close status into a 0/-1 return; an optional out pointer is
 * verified against the read-back via mem_compare. (Same shape as
 * elock_load_secret_to_od_87.)
 */
int frontlight_od_signal_register(uint32_t *spec, uint8_t *out_devdata)
{
    int reader;
    int read_status;
    int pub_status;
    int ret;
    uint8_t key[4];
    uint8_t devbuf[0xe];
    uint8_t selector;
    uint32_t bus;
    void *desc;

    reader = nvm_record_open();
    mem_set(devbuf, 0, 0xe);
    if (reader == 0) {
        read_status = -1;
    } else {
        read_status = (nvm_record_read(reader, devbuf, 0x30, 0xe) != 0) ? -1 : 0;
    }
    if (devbuf[0] > 1) {
        mem_set(devbuf, 0, 0xe);
        read_status = -1;
    }

    selector = (uint8_t)spec[1];
    bus      = spec[0];
    key[0] = selector;
    key[1] = 0x87;
    key[2] = 0;
    key[3] = 0;

    pub_status = -1;
    desc = od_registry_lookup(bus, *(uint32_t *)key);
    if (desc != 0 && od_resource_wait(*(void **)((char *)desc + 4), 100) == 0) {
        uint8_t *dst = *(uint8_t **)desc;
        int i;
        for (i = 0; i < 13; i++)
            dst[i] = devbuf[1 + i];
        pub_status = 0;
        key[0] = selector;
        key[1] = 0x87;
        key[2] = 0;
        {
            void *d2 = od_registry_lookup(bus, *(uint32_t *)key);
            if (d2 != 0)
                od_signal_send(*(uint32_t *)((char *)d2 + 4));
            else
                pub_status = -1;
        }
    }
    if (read_status != 0)
        pub_status = -1;

    if (reader == 0) {
        ret = -1;
    } else {
        nvm_record_release(reader);
        ret = 0;
    }
    if (pub_status != 0)
        ret = -1;

    if (out_devdata != 0) {
        int cmp = mem_compare(devbuf, out_devdata, 0xe);
        ret = (ret == 0) ? (-(int)(cmp != 0)) : -1;
    }
    return ret;
}
