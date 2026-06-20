/*
 * can.c — VanMoof power_control CAN protocol layer (Bosch M_CAN, 29-bit IDs).
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin   (image base 0x0)
 *
 * Functions (OEM address order):
 *   can_request_obj_1a4         @ 0x00006226
 *   can_request_obj_10a7        @ 0x00006260
 *   can_tx_task                 @ 0x00006292
 *   can_build_event_record      @ 0x0000664a
 *   can_request_response_8808   @ 0x000069d4
 *   can_send_obj_1a4            @ 0x00006a48
 *   can_send_obj_10a7           @ 0x00006a84
 *
 * The CAN objects use the documented 29-bit extended-ID encoding. Object ids:
 * 0x1a4 (battery/mode), 0x10a7 (charger capability), 0x8808 (request/response).
 * The comms registry (comms_registry_lookup) and the M_CAN controller driver are
 * vendor; the frame build / dispatch logic here is VanMoof.
 */

#include "power_control.h"

/*
 * can_request_obj_1a4 — acquire a TX scratch buffer for CAN object 0x1a4 and
 * return its data pointer. // 0x00006226
 *
 * OEM disassembly (0x00006226..0x0000625f):
 *
 * Builds the 3-byte object descriptor {0xa4, 0x01, 0x00} (packed little-endian
 * 0x0001a4) and looks it up in the comms registry. On a miss returns -1. It
 * then waits up to 100 ticks for the entry's buffer to become available; on
 * timeout returns -1; on success stores the entry's data pointer (entry[0]) to
 * *out and returns 0.
 */
int can_request_obj_1a4(uint32_t handle, uint8_t **out)
{
    uint8_t hdr[4] = { 0xa4, 0x01, 0x00, 0x00 };
    uint32_t *entry;

    entry = (uint32_t *)comms_registry_lookup(handle, *(uint32_t *)hdr, 0, 0);
    if (entry == 0) {
        return -1;
    }
    if (comms_wait(entry[1], 100) != 0) {
        return -1;
    }
    *out = (uint8_t *)entry[0];
    return 0;
}

/*
 * can_request_obj_10a7 — acquire a TX buffer for CAN object 0x10a7 and return its
 * data pointer (best-effort, no status). // 0x00006260
 *
 * OEM disassembly (0x00006260..0x00006290):
 *
 * Same shape as can_request_obj_1a4 but for object id 0x10a7 (descriptor
 * {0xa7, 0x10, 0x00}); on a found entry whose 100-tick wait succeeds it stores
 * the data pointer to *out, else leaves *out untouched.
 */
void can_request_obj_10a7(uint32_t handle, uint8_t **out)
{
    uint8_t hdr[4] = { 0xa7, 0x10, 0x00, 0x00 };
    uint32_t *entry;

    entry = (uint32_t *)comms_registry_lookup(handle, *(uint32_t *)hdr, 0, 0);
    if (entry != 0 && comms_wait(entry[1], 100) == 0) {
        *out = (uint8_t *)entry[0];
    }
}

/*
 * can_tx_task — the CAN transmit worker task: dequeue an application frame and
 * push it into the Bosch M_CAN controller's message-RAM TX buffer. // 0x00006292
 *
 * OEM disassembly (0x00006292..0x00006382):
 *
 * Infinite loop. Blocks on the instance's TX queue (instance[0]) with a 500-tick
 * timeout, retrying until a 16-byte frame { ctrl@+0, ext-id@+4, data@+8..15 }
 * arrives. It zeroes a 16-byte M_CAN element image, sets T0 = (id & 0x1fffffff)
 * with the extended-frame bit (0x40000000), the T1 DLC nibble = ctrl & 0xf, and
 * points the data field at the frame's 8 payload bytes. If the instance busy
 * flag (+0x115) is already set the frame is aborted (can_tx_abort). Otherwise it
 * arms the busy flag, reaches the M_CAN registers (instance+0x160), checks the
 * controller-ready bit (regs+0xcc bit0), computes the target element address
 * ((regs+0xc0 put index & 0xfffc) + msg-RAM base regs+0x200), copies the 8-byte
 * header and the 8 data bytes in, sets the add-request/interrupt bits (regs+0xe0,
 * +0x5c, +0x58, +0x54, and TXBAR at +0xd0), and waits up to 50 ticks for a
 * TX-complete notify on instance[1]. A zero or bit1-set notify aborts the frame.
 */
void can_tx_task(uint32_t *instance)
{
    uint8_t  item[16];  /* dequeued frame: ctrl@+0, 29-bit id@+4, data@+8 */
    uint8_t  hdr[16];   /* M_CAN element image: T0@+0, DLC@+6, data ptr@+8, len@+0xc */
    int      rc;
    uint32_t notify;
    int      base;
    uint32_t elem;

    if (instance == 0) {
        return;
    }
    for (;;) {
        do {
            rc = rtos_queue_receive(instance[0], item, 500);
        } while (rc != 1);

        mem_set(hdr, 0, 16);
        *(uint32_t *)(hdr + 0) =
            (*(uint32_t *)(item + 4) & 0x1fffffffu) | 0x40000000u;  /* T0: XTD ext id */
        hdr[0x0c] = 8;                              /* data copy length */
        hdr[0x06] = (uint8_t)(item[0] & 0xf);       /* T1 DLC nibble */
        *(void **)(hdr + 0x08) = item + 8;          /* data pointer */

        if (*((volatile uint8_t *)instance + 0x115) != 0) {
            can_tx_abort((uint32_t)instance);
            continue;
        }
        base = (int)instance[0x58];                 /* M_CAN regs @ instance+0x160 */
        *((volatile uint8_t *)instance + 0x114) = 0;
        *((volatile uint8_t *)instance + 0x115) = 3;
        if ((*(volatile uint32_t *)(base + 0xcc) & 1) == 0) {
            *((volatile uint8_t *)instance + 0x115) = 0;
            can_tx_abort((uint32_t)instance);
            continue;
        }
        elem = (*(volatile uint32_t *)(base + 0xc0) & 0xfffcu)
             + *(volatile uint32_t *)(base + 0x200);
        mem_copy((void *)elem, hdr, 8);
        mem_copy((void *)(elem + 8), *(void **)(hdr + 0x08), hdr[0x0c]);
        *(volatile uint32_t *)(base + 0xe0) |= 1;
        *(volatile uint32_t *)(base + 0x5c) |= 1;
        *(volatile uint32_t *)(base + 0x58) &= 0xfffffdffu;
        *(volatile uint32_t *)(base + 0x54) |= 0x200;
        *(volatile uint32_t *)(base + 0xd0) |= 1;   /* TXBAR */

        notify = rtos_task_notify_wait(instance[1], 7, 1, 0x32);
        if (notify == 0 || (notify & 2) != 0) {
            can_tx_abort((uint32_t)instance);
        }
    }
}

/*
 * can_build_event_record — build a 13-byte CAN event/command record from a
 * completed message descriptor and dispatch it by type. // 0x0000664a
 *
 * OEM disassembly (0x0000664a..0x000066ce):
 *
 * Returns -2 if either argument is null. Zeroes a 13-byte record and lays the
 * descriptor's 24-bit object id (desc+0xc) into bytes 0..2. The type byte at
 * desc+0x10 selects the path: type 1 sets flag bit 0x10 then falls into the
 * multi-frame fragmenter (can_event_dispatch, channel instance+0x594, marker 1);
 * type 2 dispatches directly; type 0 optionally copies a payload (len at +0x24,
 * src at +0x20) into the record body, sets flag 0x10, and invokes the per-
 * instance vtable callback at instance+0x5a4 with (instance+0x594, record),
 * returning 0; any other type returns -1.
 */
uint32_t can_build_event_record(int instance, int desc)
{
    uint8_t rec[13];
    char    type;
    int     plen;

    if (instance == 0 || desc == 0) {
        return 0xfffffffeu;   /* -2 */
    }

    mem_set(rec, 0, 0xd);
    rec[0] = (uint8_t)*(uint32_t *)(desc + 0xc);
    rec[1] = (uint8_t)(*(uint32_t *)(desc + 0xc) >> 8);
    rec[2] = (uint8_t)(*(uint32_t *)(desc + 0xc) >> 0x10);

    type = *(char *)(desc + 0x10);
    if (type == 1) {
        rec[3] |= 0x10;
    } else if (type != 2) {
        if (type == 0) {
            plen = *(int *)(desc + 0x24);
            if (plen != 0) {
                rec[4] = (uint8_t)plen;
                mem_copy(&rec[5], *(void **)(desc + 0x20), (uint32_t)plen);
            }
            rec[3] |= 0x10;
            /* per-instance vtable callback at instance+0x5a4, called (instance+0x594, record) */
            (*(void (**)(int, void *))(uintptr_t)(instance + 0x5a4))(instance + 0x594, rec);
            return 0;
        }
        return 0xffffffffu;   /* -1 */
    }
    can_event_dispatch((uint32_t)(instance + 0x594), desc, rec, 1);
    return 0;
}

/*
 * can_request_response_8808 — request/response transaction on CAN object 0x8808:
 * acquire a request frame, fill it from the payload, then resubmit and decode the
 * response. // 0x000069d4
 *
 * OEM disassembly (0x000069d4..0x00006a46):
 *
 * Looks up object 0x8808 (descriptor {0x08, 0x88, 0x00}); on a miss returns -1.
 * Waits up to 100 ticks; on timeout returns -1. Copies the caller's two payload
 * words into the acquired buffer, then resubmits the same object with the filled
 * buffer as the payload to obtain the response record. If a record comes back it
 * is decoded via can_build_event_record and its wait handle (record+4) released;
 * the decode status is returned. Any failure path returns -1.
 */
int can_request_response_8808(uint32_t handle, uint32_t *payload)
{
    uint8_t hdr[4] = { 0x08, 0x88, 0x00, 0x00 };
    uint32_t *entry;
    uint32_t *buf;
    void *record;
    int rc;

    entry = (uint32_t *)comms_registry_lookup(handle, *(uint32_t *)hdr, 0, 0);
    if (entry != 0 && comms_wait(entry[1], 100) == 0) {
        buf = (uint32_t *)entry[0];
        buf[0] = payload[0];
        buf[1] = payload[1];
        record = comms_registry_lookup(handle, *(uint32_t *)hdr, buf, 0);
        if (record != 0) {
            rc = (int)can_build_event_record((int)handle, (int)(uintptr_t)record);
            comms_release(*(uint32_t *)((char *)record + 4));
            return rc;
        }
    }
    return -1;
}

/*
 * can_send_obj_1a4 — encode and dispatch a CAN object 0x1a4 frame previously
 * staged by the caller. // 0x00006a48
 *
 * OEM disassembly (0x00006a48..0x00006a83):
 *
 * Looks up object 0x1a4; on a miss returns -1. Otherwise builds the event record
 * (can_build_event_record), releases the entry's wait handle (entry+4), and
 * returns the builder status.
 */
int can_send_obj_1a4(uint32_t handle)
{
    uint8_t hdr[4] = { 0xa4, 0x01, 0x00, 0x00 };
    void *entry;
    int rc;

    entry = comms_registry_lookup(handle, *(uint32_t *)hdr, 0, 0);
    if (entry == 0) {
        return -1;
    }
    rc = (int)can_build_event_record((int)handle, (int)(uintptr_t)entry);
    comms_release(*(uint32_t *)((char *)entry + 4));
    return rc;
}

/*
 * can_send_obj_10a7 — encode and dispatch a CAN object 0x10a7 frame. // 0x00006a84
 *
 * OEM disassembly (0x00006a84..0x00006abf):
 *
 * Identical to can_send_obj_1a4 but for object id 0x10a7 (descriptor
 * {0xa7, 0x10, 0x00}).
 */
int can_send_obj_10a7(uint32_t handle)
{
    uint8_t hdr[4] = { 0xa7, 0x10, 0x00, 0x00 };
    void *entry;
    int rc;

    entry = comms_registry_lookup(handle, *(uint32_t *)hdr, 0, 0);
    if (entry == 0) {
        return -1;
    }
    rc = (int)can_build_event_record((int)handle, (int)(uintptr_t)entry);
    comms_release(*(uint32_t *)((char *)entry + 4));
    return rc;
}
