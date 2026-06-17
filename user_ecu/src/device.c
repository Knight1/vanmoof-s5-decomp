/*
 * device.c — registry-backed device-record cache accessors.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   device_dispatch_command @ 0x000041a4 (inbound command dispatch + reassembly)
 *   device_mgr_reset      @ 0x00003f14  (re-arm the device block: stop timer + reset)
 *   device_apply_task     @ 0x00003fe8  (drain the command queue, apply each batch)
 *   device_fetch_cache_status9c0 @ 0x0000410c (fetch + cache a status word, then apply)
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
#include <stddef.h>

#include "device.h"
#include "registry.h"
#include "bus.h"        /* bus_session_open/transfer/commit, mem_free path */
#include "util.h"       /* vmem_set, vmem_copy, vmem_cmp, mem_free */
#include "store.h"      /* event_report, timer_remaining_ticks */
#include "i2c.h"        /* i2c_reg_write_53, i2c_tx_frame */
#include "control.h"    /* controlTask_CmdHandler (device_fetch_cache_status9c0) */

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
extern void  rtos_sem_give(void *sem, uint32_t a, uint32_t b);          /* semaphore give (0x00006ec0) */
extern int   rtos_sem_take(void *sem, uint32_t ticks);                  /* blocking take (0x00003698) */
extern void  vTaskDelay(uint32_t ticks);                                /* 0x00001bdc */

/*
 * Software-timer helpers used by device_mgr_reset:
 *   timer_remaining_ticks (0x000019f8) — VanMoof wrapper (declared in store.h):
 *     active-flag gated "expiry − xTickCount" query over a FreeRTOS Timer_t.
 *   xtimer_command (0x00006fd0) — FreeRTOS xTimerGenericCommand-style dispatch,
 *     vendor (deferred), satisfied upstream at link.
 */
extern void  xtimer_command(void *timer, int cmd, uint32_t opt_value,
                            void *higher_pri_woken, uint32_t ticks);    /* 0x00006fd0 */

/* FreeRTOS queue receive (vendor, deferred) — device_apply_task blocks here. */
extern int   xQueueReceive(void *queue, void *buf, uint32_t ticks);    /* 0x000033f0 */

/* Device-command queue + apply-task constants (literals @ 0x40cc/0x40d0). */
#define DEV_APPLY_QUEUE       (*(void *volatile *)0x200006a0u) /* batch-ptr queue */
#define DEV_APPLY_EVENT_CTX   0x4dfc053cu   /* event subsystem id              */
#define DEV_APPLY_EVENT_CODE  0x1au         /* event code on miss/busy/failure */
#define DEV_TAG_03C0          0x03c0u       /* lookup tag {0xc0,0x03,0x00}      */

/*
 * Device-command batch slot — 0x20 bytes; device_apply_task drains 40 (0x500
 * bytes) per dequeued batch, taking the signed 16-bit field at +0x2/+0xa/+0x12/
 * +0x1a of each slot (the OEM loads each as two bytes then sxth).
 */
typedef struct dev_cmd_slot {
    uint8_t  _b00[2];      /* +0x00 */
    int16_t  v0;           /* +0x02 */
    uint8_t  _b04[6];      /* +0x04 */
    int16_t  v1;           /* +0x0a */
    uint8_t  _b0c[6];      /* +0x0c */
    int16_t  v2;           /* +0x12 */
    uint8_t  _b14[6];      /* +0x14 */
    int16_t  v3;           /* +0x1a */
    uint8_t  _b1c[4];      /* +0x1c..+0x1f (slot is 0x20 bytes) */
} dev_cmd_slot_t;

/* device_send_chunked is a separate OEM function (0x8538); keep it un-inlined. */
void device_send_chunked(dev_txch_t *txch, dev_record_t *rec,
                         dev_frame_t *frame, uint32_t first);

/* Device-class lookup tags (low 3 bytes of registry_lookup_value's key word). */
#define DEV_TAG_TYPE87    0x87u
#define DEV_TAG_TYPE91    0x91u
#define DEV_TAG_8C0       0x08c0u
#define DEV_TAG_8808      0x8808u

/* Subsystem id stamped into the dispatcher's event/error records. // pool @0x4428 */
#define DEVDISPATCH_EVENT_CTX   0x312d3f0fu

/*
 * Reassembly slot — one of three fixed entries in the table at mgr+0x5ac
 * (0x4c-byte stride, 0x13 words). The dispatcher accumulates a multi-fragment
 * command into `buf` (cap 0x40) before invoking the record handler.
 */
typedef struct dispatch_slot {
    uint32_t age;        /* +0x00 0 = free; while busy it is also an aging count */
    uint8_t  key[3];     /* +0x04 the 3-byte command key being reassembled       */
    uint8_t  _pad7;      /* +0x07                                                */
    uint32_t fill;       /* +0x08 bytes written so far / sub-fragment write offset */
    uint8_t  buf[0x40];  /* +0x0c reassembly buffer                              */
} dispatch_slot_t;       /* 0x4c bytes */

#define DISPATCH_SLOT_COUNT   3
#define DISPATCH_BUF_CAP      0x40u

/*
 * Dispatch-record fields beyond the documented dev_record_t names. The inbound
 * dispatcher reuses the same 0x2c-byte slot the cache accessors return, but
 * reads it through these roles:
 *   +0x10 type    (0 single, 1/2 streaming)        — dev_record_t.type
 *   +0x0f mode    (0 normal, 1 raw-only)
 *   +0x08 length  (expected payload size)          — dev_record_t.length
 *   +0x14 handler  int (*)(ctx, msg, buf, len)
 *   +0x18 ctx     handler context
 *   +0x20 aux / +0x24 aux_len                      — dev_record_t.aux/aux_len
 *   +0x28 notify  semaphore released on a type-1 plain ack
 *   +0x04 sem     access semaphore                 — dev_record_t.sem
 */
#define DR_MODE(r)     (*(const uint8_t  *)((const uint8_t *)(r) + 0x0f))
#define DR_LENGTH(r)   (*(const uint32_t *)((const uint8_t *)(r) + 0x08))
#define DR_HANDLER(r)  (*(int (**)(void *, void *, void *, uint32_t)) \
                            ((const uint8_t *)(r) + 0x14))
#define DR_HCTX(r)     (*(void *const *)((const uint8_t *)(r) + 0x18))
#define DR_AUX(r)      (*(uint8_t *const *)((const uint8_t *)(r) + 0x20))
#define DR_AUXLEN(r)   (*(const uint32_t *)((const uint8_t *)(r) + 0x24))
#define DR_NOTIFY(r)   (*(void *const *)((const uint8_t *)(r) + 0x28))

/*
 * device_dispatch_command — inbound command dispatch + fragment reassembly.
 * // 0x000041a4
 *
 * `msg` is the inbound command: bytes 0..2 a 3-byte key, byte 3 flags
 * (bit 0x10 = streaming/ack qualifier, low 3 bits = fragment sub-index), byte 4
 * the payload length, bytes 5.. the payload. The key selects a device record
 * (registry_lookup_value(mgr, key24)); the record's type at +0x10 drives the
 * dispatch:
 *
 *   type 0 — single-shot. With a handler (rec+0x14) and flags bit 0x10 set,
 *            run handler(ctx, msg, buf, len): the buf/len are the rec's aux
 *            payload (refilled from the message) or, with no aux, the rec's
 *            zeroed data buffer. With a small record (length <= 8) the payload
 *            is staged in an 8-byte scratch and the handler called directly.
 *            Larger records accumulate fragments into a 3-slot reassembly table
 *            at mgr+0x5ac until rec->length bytes are gathered, then run the
 *            handler. A handler return of -3 short-circuits to success.
 *   type 1 — streaming. mode (rec+0x0f) gates flags bit 0x10; a plain ack with
 *            no handler simply releases the notify semaphore (rec+0x28).
 *   type 2 — streaming, send-only.
 *
 * When the dispatch leaves `send` armed (and the record is not type 2) a reply
 * frame is built from the key and pushed through the manager's channel
 * (device_send_chunked on mgr+0x594) under the access semaphore. Returns 0 on
 * success, 0xffffffff (-1) on a bad key/type/flags or a semaphore timeout.
 */
uint32_t device_dispatch_command(int mgr, void *msg)
{
    uint8_t *m = (uint8_t *)msg;
    uint8_t *rec;
    uint32_t key24;
    uint8_t  type;
    uint8_t  mode;
    uint8_t  send;          /* bVar11 — reply-frame gate */
    int      hret;
    dev_frame_t frame;      /* the reply frame, built at the end (sp+0x10) */

    key24 = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16);
    rec = (uint8_t *)registry_lookup_value((registry_t *)mgr, key24, 0);
    if (rec == (uint8_t *)0) {
        return 0xffffffff;
    }

    type = *(rec + 0x10);
    if (type < 2) {
        mode = DR_MODE(rec);
        if (mode == 0) {
            send = (uint8_t)(m[3] & 0x10);
            if ((m[3] & 0x10) != 0) {
                return 0xffffffff;
            }
            if (type == 1) {
                if (DR_NOTIFY(rec) == (void *)0) {
                    return 0;
                }
                rtos_sem_give(DR_NOTIFY(rec), send, send);   /* send == 0 here */
                return 0;
            }
            /* type 0 — fall through */
        } else if (mode == 1) {
            send = 1;
            if ((m[3] & 0x10) == 0) {        /* lsls #0x1b / bpl: bit4 must be set */
                return 0xffffffff;
            }
        } else {
            return 0xffffffff;
        }
    } else if (type == 2) {
        send = 1;
    } else {
        return 0xffffffff;
    }

    if (DR_HANDLER(rec) == (int (*)(void *, void *, void *, uint32_t))0) {
        goto reply;                          /* no handler: just send the reply */
    }

    if (type == 0 && (m[3] & 0x10) != 0) {
        /* single-shot with handler: pass the rec's aux/data buffer */
        uint8_t *buf;
        uint32_t len;
        if (DR_AUX(rec) != (uint8_t *)0) {
            vmem_set(DR_AUX(rec), 0, DR_AUXLEN(rec));
            vmem_copy(DR_AUX(rec), m + 5, m[4]);
            buf = DR_AUX(rec);
            len = DR_AUXLEN(rec);
        } else {
            vmem_set(*(uint8_t **)rec, 0, DR_LENGTH(rec)); /* clear rec->data (rec+0) */
            buf = *(uint8_t **)rec;                        /* rec->data */
            len = DR_LENGTH(rec);
        }
        hret = DR_HANDLER(rec)(DR_HCTX(rec), msg, buf, len);
    } else if (DR_LENGTH(rec) < 9) {
        /* small record: stage <=8 bytes on the stack and dispatch directly */
        uint8_t scratch[8];
        if ((m[3] & 7) != 0) {
            goto reply;
        }
        vmem_copy(scratch, m + 5, 8);
        hret = DR_HANDLER(rec)(DR_HCTX(rec), msg, scratch, m[4]);
    } else {
        /* multi-fragment reassembly across the 3-slot table at mgr+0x5ac */
        dispatch_slot_t *table = *(dispatch_slot_t **)((uint8_t *)mgr + 0x5ac);
        uint8_t  flags = m[3];
        uint32_t off = (uint32_t)(flags & 7) * 8;    /* sub-fragment write offset */
        dispatch_slot_t *slot = (dispatch_slot_t *)0;
        int i;

        /* find a slot already reassembling this key */
        for (i = 0; i < DISPATCH_SLOT_COUNT; i++) {
            dispatch_slot_t *s = &table[i];
            if (s->age != 0 && vmem_cmp(s->key, msg, 3) == 0) {
                if (off == s->fill) {
                    slot = s;
                    goto accumulate;             /* contiguous: append */
                }
                if ((flags & 7) != 0) {
                    return 0;                    /* non-contiguous fragment: drop */
                }
                /* restart this slot for a fresh first fragment */
                s->fill = off;                   /* == 0 */
                vmem_set(s->buf, 0, DISPATCH_BUF_CAP);
                slot = s;
                goto accumulate;
            }
        }

        if ((flags & 7) != 0) {
            return 0;                            /* continuation with no slot */
        }

        /* allocate a slot for a new first fragment */
        if (table[0].age == 0) {
            slot = &table[0];
        } else if (table[1].age == 0) {
            slot = &table[1];
        } else if (table[2].age == 0) {
            slot = &table[2];
        } else {
            /* all busy: age every slot, evict the most-aged */
            dispatch_slot_t *oldest = (dispatch_slot_t *)0;
            uint32_t boff = 0;
            do {
                dispatch_slot_t *s = (dispatch_slot_t *)((uint8_t *)table + boff);
                uint32_t a = s->age + 1;
                s->age = a;
                if (oldest == (dispatch_slot_t *)0) {
                    oldest = s;                  /* first iteration latches slot 0 */
                } else if (a > oldest->age) {
                    oldest = s;
                }
                boff += 0x4c;
            } while (boff != 0xe4);
            event_report(DEVDISPATCH_EVENT_CTX, 0xd8, 3,
                         (uint32_t)oldest->key[2],
                         (uint32_t)oldest->key[1],
                         (uint32_t)oldest->key[0]);
            slot = oldest;
        }
        slot->age  = 1;
        slot->fill = 0;
        vmem_copy(slot->key, msg, 3);
        vmem_set(slot->buf, 0, DISPATCH_BUF_CAP);

    accumulate:
        {
            uint32_t fill = slot->fill;
            if (fill + (uint32_t)m[4] > DISPATCH_BUF_CAP) {
                slot->age = 0;
                event_report(DEVDISPATCH_EVENT_CTX, 0xee, 3,
                             fill, (uint32_t)m[4], DISPATCH_BUF_CAP);
                return 0;
            }
            vmem_copy((uint8_t *)slot + fill + 0xc, m + 5, m[4]);
            slot->fill = fill + m[4];
            if (slot->fill != DR_LENGTH(rec)) {
                return 0;                        /* still gathering */
            }
            /* r3 (4th arg) survives as the just-written fill == DR_LENGTH */
            hret = DR_HANDLER(rec)(DR_HCTX(rec), msg, slot->buf, slot->fill);
            slot->age = 0;                       /* release slot */
        }
    }

    if (hret == -3) {
        return 0;
    }

reply:
    if (send != 0 && *(rec + 0x10) != 2) {
        uint8_t  rtype = *(rec + 0x10);
        uint32_t gate;

        vmem_set(&frame, 0, 0xd);
        frame.key[0] = m[0];
        frame.key[1] = m[1];
        frame.key[2] = m[2];
        frame.seq &= 0xef;                       /* bfc #4,#1 on the (zeroed) byte */

        if (rtype == 1) {
            gate = 0;
        } else {
            gate = 1;
            if (xQueueSemaphoreTake(*(void **)(rec + 0x4), 0x7fffffff) != 0) {
                event_report(DEVDISPATCH_EVENT_CTX, 0x172, 0);
                return 0xffffffff;
            }
        }
        device_send_chunked((dev_txch_t *)((uint8_t *)mgr + 0x594),
                            (dev_record_t *)rec, &frame, gate);
        if (rtype != 1) {
            rtos_sem_give_dispatch(*(void **)(rec + 0x4));
        }
    }
    return 0;
}

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

/*
 * Device peripheral block (memory-mapped, off-image at 0x4008c000). Only the
 * three control bytes cleared on entry are touched here; the full register map
 * lives in the vendor IOM/device driver. The reset register at 0x4000038c is
 * the device-block soft reset: writing 0x40000000 asserts it (the bring-up path
 * at 0x1710 writes 0 to release it).
 */
#define DEVBLK_BASE          0x4008c000u
#define DEVBLK_RESET_REG     0x4000038cu
#define DEVBLK_RESET_ASSERT  0x40000000u

/* Device-manager RAM state (shared with the control task and the sensor read). */
#define DEV_SEM_HANDLE       0x20001718u   /* access semaphore handle cell      */
#define DEV_STATE_PAIR       0x200007fcu   /* 2-word device state {mode, 0}     */
#define DEV_ENABLE_FLAG      0x200070e8u   /* device-enabled flag (u8)          */
#define DEV_SENSOR_VARIANT   0x200070dfu   /* sensor sub-address selector (u8)  */
#define DEV_TIMER_HANDLE     0x20001d84u   /* software-timer handle cell        */
#define DEV_TIMER_REMAINING  0x200006a4u   /* last queried timer remaining/state */

/* Subsystem id stamped into device_mgr_reset's event/error records. // pool @0x3fd8 */
#define DEVRESET_EVENT_CTX   0xc8cf5962u

/*
 * device_mgr_reset — re-arm the device block: stop its timer, reprogram the
 * part and assert the peripheral soft reset. // 0x00003f14
 *
 * Clears three device-block control bytes (off 0x04/0x17/0x19), takes the access
 * semaphore (blocking, portMAX_DELAY), zeroes the device state pair and raises
 * the enable flag, then programs the part: a register write (opcode 0x53,
 * value 1) followed by a two-byte command frame {0x36, 0x15}. A failed register
 * write reports event 0x4a; a successful frame send is followed by a 5-tick
 * delay, a failed one reports event 0x4f. It then latches the sensor variant
 * (0xe0) and the state pair {0xe0, 0}, releases the semaphore, and queries the
 * software timer: the remaining-tick value (or 1 when zero) is stashed at
 * DEV_TIMER_REMAINING and, if the timer is live, it is stopped (command 5) and
 * its handle cleared. Finally the device-block soft reset is asserted. Always
 * returns 0. Reached as a device-manager dispatch callback (no direct callers).
 *
 * The OEM `push {r0-r3,...}` only reserves the 4-word scratch frame; none of the
 * caller's register arguments are read, so this is a no-argument routine.
 */
int device_mgr_reset(void)
{
    uint8_t  cmd[2];                 /* sp+0xc scratch command buffer */
    int      status;
    void   **timer;                  /* DEV_TIMER_HANDLE: pointer to the timer object */
    int      remaining;

    /* 0x3f1c-0x3f20: clear the three device-block control bytes */
    *(volatile uint8_t *)(DEVBLK_BASE + 0x19) = 0;
    *(volatile uint8_t *)(DEVBLK_BASE + 0x04) = 0;
    *(volatile uint8_t *)(DEVBLK_BASE + 0x17) = 0;

    /* 0x3f22-0x3f28: take the access semaphore, block forever */
    rtos_sem_take(*(void *volatile *)DEV_SEM_HANDLE, 0xffffffffu);

    /* 0x3f2e-0x3f3a: reset the state pair, raise the enable flag */
    ((volatile uint32_t *)DEV_STATE_PAIR)[0] = 0;       /* strd r4,r4 */
    ((volatile uint32_t *)DEV_STATE_PAIR)[1] = 0;
    *(volatile uint8_t *)DEV_ENABLE_FLAG = 1;

    /* 0x3f3c-0x3f4e: register write (opcode 0x53, value 1); report on failure */
    cmd[0] = 0;                                          /* strb r4,[sp,#0xc] */
    status = i2c_reg_write_53(0, (uint32_t)(uintptr_t)cmd, 1);
    if (status != 0) {
        event_report(DEVRESET_EVENT_CTX, 0x4a, 0);       /* 0x3f4c */
    }

    /* 0x3f50-0x3f64: send the 2-byte command frame {0x36, 0x15} */
    cmd[0] = 0x36;
    cmd[1] = 0x15;
    if (i2c_tx_frame(cmd, 2) == 0) {
        vTaskDelay(5);                                   /* 0x3f68 */
    } else {
        event_report(DEVRESET_EVENT_CTX, 0x4f, 0);       /* 0x3fba */
    }

    /* 0x3f6c-0x3f7c: latch sensor variant + state pair, release the semaphore */
    *(volatile uint8_t *)DEV_SENSOR_VARIANT = 0xe0;      /* strb [r1] */
    ((volatile uint32_t *)DEV_STATE_PAIR)[0] = 0xe0;     /* strd r3(0xe0),r2(0) */
    ((volatile uint32_t *)DEV_STATE_PAIR)[1] = 0;
    rtos_sem_give(*(void *volatile *)DEV_SEM_HANDLE, 0, 0);

    /* 0x3f80-0x3fc4: query the software timer; stash remaining (or 1), stop it */
    timer = *(void ** volatile *)DEV_TIMER_HANDLE;
    remaining = timer_remaining_ticks(timer);
    *(volatile int *)DEV_TIMER_REMAINING = (remaining == 0) ? 1 : remaining;
    if (timer != NULL && *timer != NULL) {
        xtimer_command(*timer, 5, 0, NULL, 0);           /* 0x3fa0 */
        *timer = NULL;                                   /* 0x3fa4 */
    }

    /* 0x3fa6-0x3fac: assert the device-block soft reset */
    *(volatile uint32_t *)DEVBLK_RESET_REG = DEVBLK_RESET_ASSERT;
    return 0;
}

/*
 * device_apply_task — drain the device-command queue and push each batch to the
 * registry device. // 0x00003fe8
 *
 * A never-returning FreeRTOS task. It blocks on the global command queue
 * (DEV_APPLY_QUEUE); each message is a pointer to a 0x500-byte batch of forty
 * 0x20-byte command slots. For each slot it extracts four signed 16-bit values,
 * looks up the target device record by tag {0xc0,0x03,0x00}, takes its
 * semaphore, writes the four halfwords into the cached buffer, re-looks-up the
 * record, runs device_apply and releases the semaphore. Any miss/busy/apply
 * failure posts event_report(ctx, 0x1a, 0). (Lookups pass key1 = 0, the OEM's
 * don't-care r2.)
 */
void device_apply_task(void *mgr)
{
    registry_t *reg   = (registry_t *)mgr;             /* r4 = param */
    void       *queue = DEV_APPLY_QUEUE;               /* r11 = *0x200006a0 */

    for (;;) {
        const dev_cmd_slot_t *batch;

        /* block until a batch pointer is dequeued (portMAX_DELAY) */
        while (xQueueReceive(queue, &batch, 0xffffffffu) != 1) {
        }

        const dev_cmd_slot_t *slot = batch;
        const dev_cmd_slot_t *end  = batch + 0x28;     /* +0x500 == 40 slots */

        do {
            int16_t v0 = slot->v0;                     /* ldrsh slot+0x02 */
            int16_t v1 = slot->v1;                     /* ldrsh slot+0x0a */
            int16_t v2 = slot->v2;                     /* ldrsh slot+0x12 */
            int16_t v3 = slot->v3;                     /* ldrsh slot+0x1a */
            dev_record_t *rec;

            rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_03C0, 0);
            if (rec != (dev_record_t *)0 &&
                xQueueSemaphoreTake(rec->sem, 100) == 0) {
                int16_t *data = (int16_t *)rec->data;   /* rec+0x00 -> buffer */
                data[2] = v2;                           /* strh data+0x04 */
                data[0] = v0;                           /* strh data+0x00 */
                data[1] = v1;                           /* strh data+0x02 */
                data[3] = v3;                           /* strh data+0x06 */

                rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_03C0, 0);
                if (rec != (dev_record_t *)0) {
                    int rc = device_apply(reg, rec);
                    rtos_sem_give_dispatch(rec->sem);   /* release semaphore */
                    if (rc != 0) {
                        event_report(DEV_APPLY_EVENT_CTX, DEV_APPLY_EVENT_CODE, 0);
                    }
                } else {
                    event_report(DEV_APPLY_EVENT_CTX, DEV_APPLY_EVENT_CODE, 0);
                }
            } else {
                event_report(DEV_APPLY_EVENT_CTX, DEV_APPLY_EVENT_CODE, 0);
            }

            slot++;                                     /* += 0x20 */
        } while (slot != end);
    }
}

/*
 * device_fetch_cache_status9c0 — fetch a status word and cache it into device
 * {..,0x09,0xc0}, then run the apply/notify hook. // 0x0000410c
 *
 * Gated on cmd[0] == 1. Pulls a 16-bit status via controlTask_CmdHandler(0x3a);
 * on a handler error it posts an event and returns the sign-extended error.
 * Otherwise it looks up the device keyed {0x09,0xc0} (top byte of `ctx` stamped
 * into the key word), takes its semaphore, stores the status into the cached
 * record, re-looks-up the slot and runs device_apply + releases the semaphore.
 * Returns 0, or the sign-extended handler error.
 */
int device_fetch_cache_status9c0(void *reg, uint32_t ctx, const char *cmd, uint32_t arg)
{
    int           rc;
    uint32_t      out[2];   /* controlTask_CmdHandler result {value, status} */
    uint32_t      key0;     /* {0x09,0xc0} tag, ctx high byte stamped */
    dev_record_t *e1;
    dev_record_t *e2;

    if (*cmd != 1) {                              /* ldrb r5; cmp #1; bne */
        return 0;
    }

    /* result struct pre-seeded with {cmd, arg} (the OEM's arg spill); cmd 0x3a
     * only produces out[0] (the read status). */
    out[0] = (uint32_t)(uintptr_t)cmd;
    out[1] = arg;
    rc = controlTask_CmdHandler(0x3a, out, 0, 0);  /* 0x411c: bl 0x1ee0 */
    if (rc != 0) {
        event_report(0xc8cf5962u, 0x34, 1, (uint32_t)rc);   /* 0x412c: bl 0x3eac */
        return (int)(int8_t)rc;                              /* 0x4130: sxtb r0,r4 */
    }

    key0 = (ctx & 0xff000000u) | 0x000009c0u;     /* low 3 bytes {0xc0,0x09,0x00} */

    e1 = (dev_record_t *)registry_lookup_value(reg, key0, (uint32_t)(uintptr_t)cmd);
    if (e1 != (dev_record_t *)0 &&
        xQueueSemaphoreTake(e1->sem, 100) == 0) {            /* take(e1[1],0x64) */
        *(uint16_t *)e1->data = (uint16_t)out[0];            /* cache status */
        e2 = (dev_record_t *)registry_lookup_value(reg, key0, (uint16_t)out[0]);
        if (e2 != (dev_record_t *)0) {
            device_apply(reg, e2);                           /* 0x4192: bl 0x8656 */
            rtos_sem_give_dispatch(e2->sem);                 /* 0x4198: bl 0x97f4 */
        }
    }
    return 0;
}
