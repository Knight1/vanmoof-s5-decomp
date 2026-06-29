/*
 * util.c — VanMoof S5 motor_sensor: subscription table, ring buffers, GATT.
 *
 * The flat CAN/GATT subscription list, the byte ring buffers that carry frame
 * payloads, and the GATT attribute-write dispatcher (with its 3-slot fragment
 * reassembly engine). Translated from the OEM image (LPC546xx-class, base 0x0);
 * the FreeRTOS critical-section + memcpy primitives are vendor.
 *
 * OEM: subscription_list_add 0x52d2, ringbuf_write_produce 0x5278,
 * ringbuf_read_consume 0x520a, gatt_attr_write_dispatch 0x2ee8.
 */

#include "motor_sensor.h"

extern int subscription_list_search(subscription_list_t *list, const void *key); /* 0x4cfa */

/* ------------------------------------------------------------------- 0x52d2 */

/* Append a 0x2c-byte entry to a flat subscription list; reject NULL, full, or
 * duplicate (key at entry+0xc). Returns 0 / -1. */
int32_t subscription_list_add(subscription_list_t *list, const void *entry, uint32_t p3, uint32_t p4)
{
    (void)p3; (void)p4;
    if (list == NULL)
        return -1;
    if (list->count >= list->capacity)
        return -1;
    if (subscription_list_search(list, (const uint8_t *)entry + 0x0c) >= 0)
        return -1;                                     /* duplicate key */

    mem_cpy((uint8_t *)list->data + list->count * SUB_ENTRY_STRIDE, entry, SUB_ENTRY_STRIDE);
    list->count++;
    return 0;
}

/* ------------------------------------------------------------------- 0x5278 */

/* Produce len bytes into a circular ring buffer, splitting across the wrap.
 * Returns len. */
uint32_t ringbuf_write_produce(ring_buf_t *rb, const uint8_t *src, uint32_t len)
{
    uint32_t first = rb->capacity - rb->write_ptr;
    if (first >= len) {
        mem_cpy(rb->buf + rb->write_ptr, src, len);
    } else {
        mem_cpy(rb->buf + rb->write_ptr, src, first);
        mem_cpy(rb->buf, src + first, len - first);
    }
    rb->write_ptr += len;
    if (rb->write_ptr >= rb->capacity)
        rb->write_ptr -= rb->capacity;
    return len;
}

/* ------------------------------------------------------------------- 0x520a */

/* Consume up to min(avail, req) bytes from a circular ring buffer into dst.
 * `avail` is supplied by the caller. Returns bytes read. */
uint32_t ringbuf_read_consume(ring_buf_t *rb, uint8_t *dst, uint32_t avail, uint32_t req)
{
    uint32_t n = (avail < req) ? avail : req;
    uint32_t first = rb->capacity - rb->read_ptr;
    if (first >= n) {
        mem_cpy(dst, rb->buf + rb->read_ptr, n);
    } else {
        mem_cpy(dst, rb->buf + rb->read_ptr, first);
        mem_cpy(dst + first, rb->buf, n - first);
    }
    rb->read_ptr += n;
    if (rb->read_ptr >= rb->capacity)
        rb->read_ptr -= rb->capacity;
    return n;
}

/* ------------------------------------------------------------------- 0x2ee8 */

/* GATT attribute-write dispatcher. Resolves the 3-byte UUID against the
 * subscription table and dispatches by attr type: 0 plain (call handler),
 * 1 read-only-notify, 2 fragmented (3-slot, 64-byte reassembly with LRU
 * eviction). On completion of a subscribed/fragmented write sends an ACK and
 * signals the entry timer. Behaviour-oriented model of the OEM body. */
int32_t gatt_attr_write_dispatch(void *ctx, void *req_)
{
    uint8_t *req   = (uint8_t *)req_;
    uint8_t *uuid  = req + 0;                          /* +0x00 3-byte UUID */
    uint8_t  flags = req[3];                           /* +0x03 */
    uint8_t  plen  = req[4];                           /* +0x04 */
    uint8_t *payload = req + 5;
    sub_entry_t *e;

    e = (sub_entry_t *)(intptr_t)subscription_find_entry_or_null(ctx, uuid, NULL, NULL, NULL);
    if (e == NULL)
        return -1;

    switch (e->state) {                                /* attr_type at +0x10 */
    case 0: {                                          /* plain write */
        ms_handler3_fn handler = (ms_handler3_fn)e->rx_cb;   /* +0x14 */
        if (handler)
            handler(e->notify_q /*context +0x18 view*/, payload, plen);
        break;
    }
    case 1:                                            /* read-only notify */
        if (e->notify_q != NULL)
            freertos_queue_send_from_isr_or_task(e->notify_q);
        break;
    case 2: {                                          /* fragmented reassembly */
        uint8_t *slots = *(uint8_t **)((uint8_t *)ctx + 0x5ac);   /* 3 × 0x4c */
        uint8_t  frag  = flags & 0x07u;
        uint8_t *slot  = NULL;
        int i;
        (void)frag;
        /* find or allocate a slot keyed by UUID */
        for (i = 0; i < 3; i++) {
            uint8_t *s = slots + i * 0x4cu;
            if (*(uint32_t *)s != 0 &&
                s[4] == uuid[0] && s[5] == uuid[1] && s[6] == uuid[2]) { slot = s; break; }
        }
        if (slot == NULL) {                            /* allocate first free / evict */
            for (i = 0; i < 3; i++) {
                uint8_t *s = slots + i * 0x4cu;
                if (*(uint32_t *)s == 0) { slot = s; break; }
            }
            if (slot == NULL) {                        /* all full: LRU evict */
                slot = slots;
                can_diag_log_send(0, 0xd8, 0);         /* eviction diag */
            }
            *(uint32_t *)slot = 1;
            slot[4] = uuid[0]; slot[5] = uuid[1]; slot[6] = uuid[2];
            *(uint32_t *)(slot + 8) = 0;
        }
        {
            uint32_t off = *(uint32_t *)(slot + 8);
            if (off + plen > 0x40u) {                  /* overflow */
                *(uint32_t *)slot = 0;
                can_diag_log_send(0, 0xee, 0);
                return -1;
            }
            mem_cpy(slot + 0x0c + off, payload, plen);
            off += plen;
            *(uint32_t *)(slot + 8) = off;
            if (off >= e->sec_len) {                   /* assembled (primary_len) */
                ms_handler2_fn handler = (ms_handler2_fn)e->rx_cb;
                if (handler) handler(e->notify_q, slot + 0x0c);
                *(uint32_t *)slot = 0;
            }
        }
        break;
    }
    default:
        return -1;
    }

    if ((flags & (0x10u | 0x20u)) || e->state == 2) {  /* subscribed/fragmented: ACK */
        can_tx_frame_write_chunks((uint8_t *)ctx + 0x594, e->frame_buf);
        if (e->timer)
            freertos_timer_generic_command(e->timer);
    }
    return 0;
}
