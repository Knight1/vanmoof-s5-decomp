/*
 * device.c — registry-backed device-record cache accessors.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   device_send_chunked   @ 0x00008538  (≤8-byte chunked frame transmit)
 *   device_apply          @ 0x00008656  (build + transmit a record's command frame)
 *   device_read_record87  @ 0x00008e76  (read 14-byte record, tag {id,0x87})
 *   device_store_field8c0 @ 0x00008e0a  (write 3-byte field, tag {0xc0,0x08})
 *   device_cmd_read87     @ 0x00008f76  (command write+verify, then read87)
 *   device_read_record91  @ 0x00008fd6  (read 16-byte record, tag {id,0x91})
 *   device_cmd_read91     @ 0x000090c0  (command write+verify @0x40, then read91)
 *   device_store_words8808@ 0x00009178  (write 8-byte field, tag {0x08,0x88})
 *
 * Each accessor opens a transient I²C bus session, transfers a record to/from
 * the part, and refreshes the registry-cached copy under the device semaphore.
 * The registry is the device table reconstructed in registry.c; the slot
 * returned by registry_lookup_value is a 0x2c-byte record whose first two words
 * are the cached buffer pointer and the access semaphore (see dev_record).
 */

#include <stdint.h>

#include "device.h"
#include "registry.h"
#include "bus.h"        /* bus_session_open/transfer/commit, mem_free path */
#include "util.h"       /* vmem_set, vmem_copy, vmem_cmp, mem_free */

/* dev_record_t (the 0x2c-byte registry slot) is declared in device.h. */

/*
 * Device-manager transmit channel (mgr+0x594). Its send method pointer sits at
 * channel+0x10 (== mgr+0x5a4) and is invoked as send(channel, frame) for each
 * outgoing command frame.
 */
typedef struct dev_txch {
    uint8_t _b00[0x10];                              /* +0x00 */
    int (*send)(struct dev_txch *ch, void *frame);   /* +0x10 transmit one frame */
} dev_txch_t;

/*
 * Outgoing command frame (cleared to 13 bytes by device_apply): a 3-byte record
 * key, a sequence/flags byte, a length byte, then up to 8 payload bytes.
 */
typedef struct dev_frame {
    uint8_t key[3];      /* +0x00 record key {id, type, 0} */
    uint8_t seq;         /* +0x03 sequence/flags (bit 0x10 + mod-8/16 counter) */
    uint8_t len;         /* +0x04 chunk/payload length */
    uint8_t payload[8];  /* +0x05 up to 8 payload bytes (13 bytes used) */
} dev_frame_t;

/* FreeRTOS — vendor (deferred). */
extern int   xQueueSemaphoreTake(void *sem, uint32_t ticks);            /* 0x00008d54 */
extern void  rtos_sem_give_dispatch(void *sem);                         /* semaphore give (0x000097f4) */

/* device_send_chunked is a separate OEM function (0x8538); keep it un-inlined. */
void device_send_chunked(dev_txch_t *txch, dev_record_t *rec,
                         dev_frame_t *frame, uint32_t first);

/* Device-class lookup tags (low 3 bytes of registry_lookup_value's key word). */
#define DEV_TAG_TYPE87    0x87u
#define DEV_TAG_TYPE91    0x91u
#define DEV_TAG_8C0       0x08c0u
#define DEV_TAG_8808      0x8808u

/*
 * device_send_chunked — transmit rec's payload as ≤8-byte frames. // 0x00008538
 *
 * With `first` non-zero, rec->length bytes (from rec->data) are sent in
 * ceil(length/8) frames — each frame->len = min(remaining, 8), payload copied
 * from rec->data + offset; with `first` zero a single empty frame is sent. After
 * each send the frame's sequence nibble (frame->seq) is incremented and masked:
 * mod 16 when rec->type == 2, otherwise mod 8 (the high bits are preserved).
 */
void device_send_chunked(dev_txch_t *txch, dev_record_t *rec,
                         dev_frame_t *frame, uint32_t first)
{
    uint32_t remaining = (first == 0) ? 0 : rec->length;   /* cbz r3 -> r4 = 0 */
    uint32_t offset = 0;

    do {
        uint32_t chunk = remaining;
        if (chunk > 8) {                        /* 7 < remaining -> clamp to 8 */
            chunk = 8;
        }
        frame->len = (uint8_t)chunk;            /* strb [frame,#4] */
        if (remaining != 0) {
            vmem_copy(frame->payload, rec->data + offset, frame->len);
        }
        remaining -= frame->len;                /* OEM re-reads the stored len byte */
        offset    += frame->len;
        txch->send(txch, frame);                /* (*(channel+0x10))(channel, frame) */

        {
            uint8_t next = (uint8_t)(frame->seq + 1);   /* add r2,r3,#1 */
            if (rec->type == 2) {
                frame->seq = (uint8_t)((frame->seq & 0xf0u) | (next & 0x0fu)); /* bfi #0,#4 */
            } else {
                frame->seq = (uint8_t)((frame->seq & 0xf8u) | (next & 0x07u)); /* bfi #0,#3 */
            }
        }
    } while (remaining != 0);
}

/*
 * device_apply — build a command frame from rec's descriptor and transmit it.
 * // 0x00008656  (prototype + summary in device.h)
 */
int device_apply(void *mgr, dev_record_t *rec)
{
    uint8_t    *m = (uint8_t *)mgr;
    dev_txch_t *txch;
    dev_frame_t frame;

    if (mgr == (void *)0 || rec == (dev_record_t *)0) {
        return -2;                              /* mvn r0,#1 = -2 */
    }

    vmem_set(&frame, 0, 0xd);                   /* clear 13-byte frame */
    frame.key[0] = (uint8_t)rec->key;           /* strh frame[0..1] = key & 0xffff */
    frame.key[1] = (uint8_t)(rec->key >> 8);
    frame.key[2] = (uint8_t)(rec->key >> 16);   /* strb frame[2] = (key >> 16) */

    txch = (dev_txch_t *)(m + 0x594);

    if (rec->type == 1) {                        /* cmp #1 ; beq */
        frame.seq |= 0x10;
        device_send_chunked(txch, rec, &frame, 1);
        return 0;
    }
    if (rec->type == 2) {                        /* cmp #2 ; beq */
        device_send_chunked(txch, rec, &frame, 1);
        return 0;
    }
    if (rec->type != 0) {                        /* cbnz -> -1 */
        return -1;
    }

    /* type 0: single-shot transmit, optionally carrying the aux payload */
    if (rec->aux_len != 0) {
        frame.len = (uint8_t)rec->aux_len;
        vmem_copy(frame.payload, rec->aux, rec->aux_len);
    }
    frame.seq |= 0x10;
    txch->send(txch, &frame);                    /* (*(mgr+0x5a4))(mgr+0x594, frame) */
    return 0;
}

/*
 * device_read_record87 — refresh device {id, 0x87} from the bus. // 0x00008e76
 *
 * Reads 14 bytes from sub-address 0x30, rejects a leading length/status byte
 * > 1, then caches the 13-byte payload (bytes 1..13) into the device record
 * under its semaphore. With `expect`, the raw 14-byte read is memcmp'd against
 * it. Returns 0 on success, -1 otherwise.
 */
int device_read_record87(dev_handle_t *dev, const void *expect)
{
    uint8_t       buf[14];
    int           read_status;
    int           copy_status;
    int           ret;
    bus_session_t *session;
    dev_record_t *rec;
    registry_t   *reg;
    uint8_t       id;

    session = bus_session_open();                       /* open bus session */
    vmem_set(buf, 0, 0xe);
    if (session == (void *)0) {
        read_status = -1;
    } else {
        read_status = -(bus_transfer(session, buf, 0x30, 0xe) != 0);
    }
    if (1 < buf[0]) {                               /* invalid length/status byte */
        vmem_set(buf, 0, 0xe);
        read_status = -1;
    }

    id  = dev->id;
    reg = dev->reg;

    /* Lookup tag {id, 0x87, 0}. registry_lookup_value re-stacks the key; only
     * the low 3 bytes are compared, so the 2nd key word is a don't-care. */
    rec = (dev_record_t *)registry_lookup_value(reg,
              (uint32_t)id | (DEV_TAG_TYPE87 << 8), 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        int i;
        /* cache the 13-byte payload (buf[1..13]); OEM inlines a 3-word+1-byte copy */
        for (i = 0; i < 13; i++) {
            data[i] = buf[1 + i];
        }
        copy_status = 0;
        rec = (dev_record_t *)registry_lookup_value(reg,
                  (uint32_t)id | (DEV_TAG_TYPE87 << 8), 0);
        if (rec != (dev_record_t *)0) {
            rtos_sem_give_dispatch(rec->sem);                 /* release the access semaphore */
        } else {
            copy_status = -1;
        }
    } else {
        copy_status = -1;
    }

    if (read_status != 0) {
        copy_status = -1;
    }
    if (session == (void *)0) {
        ret = -1;
    } else {
        mem_free(session);                      /* close session */
        ret = 0;
    }
    if (copy_status != 0) {
        ret = -1;
    }
    if (expect != (const void *)0) {
        int diff = vmem_cmp(buf, expect, 0xe);
        if (ret == 0) {
            ret = -(diff != 0);
        } else {
            ret = -1;
        }
    }
    return ret;
}

/*
 * device_store_field8c0 — write a 3-byte field into device {0xc0,0x08}. // 0x00008e0a
 *
 * Looks up the device, takes its semaphore, stores src[0..2] into the cached
 * record, runs the apply/notify hook and releases the semaphore (OEM tail-calls
 * the give). `arg2` mirrors the OEM's unused 3rd register argument.
 */
void device_store_field8c0(registry_t *reg, const void *src, uint32_t arg2)
{
    const uint8_t *s = (const uint8_t *)src;
    dev_record_t  *rec;

    (void)arg2;     /* OEM leaves r2 (the 2nd key word) as the caller's; unused */

    rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8C0, 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        /* store the 3-byte field (OEM: strh src[0..1] + strb src[2]) */
        data[0] = s[0];
        data[1] = s[1];
        data[2] = s[2];
        rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8C0, 0);
        if (rec != (dev_record_t *)0) {
            device_apply(reg, rec);                 /* apply/notify hook */
            rtos_sem_give_dispatch(rec->sem);                 /* release semaphore (tail call) */
        }
    }
}

/*
 * device_cmd_read87 — command write-then-read of device {dev->id, 0x87}. // 0x00008f76
 *
 * Builds a 14-byte frame (0x01 || payload[13]), writes it with read-back verify
 * (bus_page_write_verify), commits the session (bus_session_commit), then refreshes the device
 * via device_read_record87 with the frame as the expected value. `arg2` mirrors
 * the OEM's unused 2nd register argument. Returns the read result, or -1 on any
 * session/write failure.
 */
int device_cmd_read87(dev_handle_t *dev, uint32_t arg2, const void *payload)
{
    uint8_t  frame[14];
    bus_session_t *session;
    int      write_status;
    int      commit_status;
    int      read_result;

    (void)arg2;     /* OEM never reads r1 (the 2nd argument) */

    session = bus_session_open();
    if (session != (void *)0) {
        frame[0] = 1;
        vmem_copy(&frame[1], payload, 0xd);                  /* 13-byte payload */
        write_status  = bus_page_write_verify(session, frame, 0xe, 0); /* write + verify */
        commit_status = bus_session_commit(session, 1);            /* commit/flush */
        if (write_status == 0) {
            write_status = -(commit_status != 0);
        } else {
            write_status = -1;
        }
        mem_free(session);                               /* close session */
        read_result = device_read_record87(dev, frame);
        if (write_status == 0) {
            return read_result;
        }
    }
    return -1;
}

/*
 * device_read_record91 — refresh device {id, 0x91} from the bus. // 0x00008fd6
 *
 * Reads 16 bytes from sub-address 0x40 and caches the full 16-byte record into
 * the device record under its semaphore (no length-byte gate, unlike the 0x87
 * reader). With `expect`, the raw 16-byte read is memcmp'd against it. Returns 0
 * on success, -1 otherwise.
 */
int device_read_record91(dev_handle_t *dev, const void *expect)
{
    uint8_t       buf[16];
    int           read_status;
    int           copy_status;
    int           ret;
    bus_session_t *session;
    dev_record_t *rec;
    registry_t   *reg;
    uint8_t       id;

    session = bus_session_open();
    vmem_set(buf, 0, 0x10);
    if (session == (void *)0) {
        read_status = -1;
    } else {
        read_status = -(bus_transfer(session, buf, 0x40, 0x10) != 0);
    }

    id  = dev->id;
    reg = dev->reg;

    rec = (dev_record_t *)registry_lookup_value(reg,
              (uint32_t)id | (DEV_TAG_TYPE91 << 8), 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        int i;
        /* cache all 16 bytes; OEM inlines a 2-words-per-iteration copy */
        for (i = 0; i < 16; i++) {
            data[i] = buf[i];
        }
        copy_status = 0;
        rec = (dev_record_t *)registry_lookup_value(reg,
                  (uint32_t)id | (DEV_TAG_TYPE91 << 8), 0);
        if (rec != (dev_record_t *)0) {
            rtos_sem_give_dispatch(rec->sem);
        } else {
            copy_status = -1;
        }
    } else {
        copy_status = -1;
    }

    if (read_status != 0) {
        copy_status = -1;
    }
    if (session == (void *)0) {
        ret = -1;
    } else {
        mem_free(session);
        ret = 0;
    }
    if (copy_status != 0) {
        ret = -1;
    }
    if (expect != (const void *)0) {
        int diff = vmem_cmp(buf, expect, 0x10);
        if (ret == 0) {
            ret = -(diff != 0);
        } else {
            ret = -1;
        }
    }
    return ret;
}

/*
 * device_cmd_read91 — command write-then-read of device {dev->id, 0x91}. // 0x000090c0
 *
 * Opens a session, writes the 16-byte `payload` into the device's 0x40 record
 * region (bus_page_write_verify with off 0x10 → page +0x40, read-back verified),
 * commits the session, then refreshes the device via device_read_record91 with
 * the payload as the expected value. `arg2` mirrors the OEM's unused 2nd register
 * argument. Returns the read result, or -1 on any session/write failure. The
 * 16-byte twin of device_cmd_read87 (which targets the 0x30 region).
 */
int device_cmd_read91(dev_handle_t *dev, uint32_t arg2, const void *payload)
{
    uint8_t  frame[16];
    bus_session_t *session;
    int      write_status;
    int      commit_status;
    int      read_result;

    (void)arg2;     /* OEM never reads r1 (the 2nd argument) */

    session = bus_session_open();
    if (session != (bus_session_t *)0) {
        vmem_copy(frame, payload, 0x10);                       /* 16-byte payload */
        write_status  = bus_page_write_verify(session, frame, 0x10, 0x10); /* write + verify @0x40 */
        commit_status = bus_session_commit(session, 1);        /* commit/flush */
        if (write_status == 0) {
            write_status = -(commit_status != 0);
        } else {
            write_status = -1;
        }
        mem_free(session);                                     /* close session */
        read_result = device_read_record91(dev, frame);
        if (write_status == 0) {
            return read_result;
        }
    }
    return -1;
}

/*
 * device_store_words8808 — write an 8-byte field into device {0x08,0x88}. // 0x00009178
 *
 * Looks up the device, takes its semaphore, stores `src`[0..1] (two words) into
 * the cached record, then runs device_apply and releases the semaphore. Returns
 * device_apply's status, or -1 if the device is absent or busy. `arg3` mirrors
 * the OEM's unused key-word argument. Sibling of device_store_field8c0 (the
 * 3-byte variant), but it forwards the apply result.
 */
int device_store_words8808(registry_t *reg, const void *src, uint32_t arg3)
{
    const uint32_t *s = (const uint32_t *)src;
    dev_record_t   *rec;

    (void)arg3;     /* OEM leaves r2 (the 2nd key word) as the caller's; unused */

    rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8808, 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint32_t *data = (uint32_t *)rec->data;
        /* store an 8-byte field (OEM: two word stores) */
        data[0] = s[0];
        data[1] = s[1];
        rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8808, 0);
        if (rec != (dev_record_t *)0) {
            int rc = device_apply(reg, rec);
            rtos_sem_give_dispatch(rec->sem);
            return rc;
        }
    }
    return -1;
}
