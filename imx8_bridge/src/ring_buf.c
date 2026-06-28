/*
 * ring_buf.c — VanMoof S5 imx8_bridge ring-buffer transport.
 *
 * The byte ring that carries a CAN-TP session's payload between the CAN side and
 * the SPI side. Translated from the OEM image (NXP LPC55S69, base 0x0). The
 * FreeRTOS critical-section / notify primitives and memcpy are vendor.
 *
 * Verified against the OEM disassembly by the deep-decode pass; the verifier's
 * corrections are folded in (the `ring_buf_read` overflow guard tests `avail`,
 * the length-prefix header is a single u32, and `ring_buf_write_notify` returns
 * true on a PARTIAL/FAILED write — all below).
 */

#include "imx8_bridge.h"

/* ------------------------------------------------------------ 0x5090 / 0x50b8 */

/* bytes currently stored: (write + capacity - read) mod capacity. */
uint32_t ring_buf_bytes_used(ring_buf_t *rb)
{
    uint32_t used = rb->write_ptr + rb->capacity - rb->read_ptr;
    if (used >= rb->capacity)
        used -= rb->capacity;
    return used;
}

/* free space, keeping one slot empty so full != empty. Panics on NULL. */
uint32_t ring_buf_bytes_free(ring_buf_t *rb)
{
    uint32_t freebytes;
    if (rb == NULL) {
        port_set_interrupt_mask();        /* configASSERT trap (spins) */
        for (;;) { }
    }
    freebytes = rb->capacity + rb->read_ptr - 1u - rb->write_ptr;
    if (freebytes >= rb->capacity)
        freebytes -= rb->capacity;
    return freebytes;
}

/* ------------------------------------------------------------ 0x52f2 / 0x5360 */

/* read up to min(want, avail) bytes, splitting across the wrap with two copies.
 * OEM 0x52f2: the final overflow guard compares `avail` (param_4), not `want`. */
uint32_t ring_buf_read(ring_buf_t *rb, void *dst, uint32_t want, uint32_t avail)
{
    uint32_t to_copy = (want < avail) ? want : avail;
    uint32_t room    = rb->capacity - rb->read_ptr;
    uint32_t first   = (to_copy < room) ? to_copy : room;   /* min(to_copy, cap-read) */
    uint8_t *d       = (uint8_t *)dst;

    if (to_copy == 0)                          /* OEM cbz r3,0x535a early-out */
        return 0;

    if (rb->capacity < rb->read_ptr + first) { /* consistency trap */
        port_set_interrupt_mask();
        for (;;) { }
    }
    mem_cpy(d, rb->data_buf + rb->read_ptr, first);

    if (first < to_copy) {                     /* wrap: copy the remainder */
        mem_cpy(d + first, rb->data_buf, to_copy - first);
    } else if (want < first) {                 /* no-wrap defensive trap */
        port_set_interrupt_mask();
        for (;;) { }
    }

    rb->read_ptr += to_copy;
    if (rb->read_ptr >= rb->capacity)
        rb->read_ptr -= rb->capacity;
    return to_copy;
}

/* write exactly count bytes (caller guarantees room); always returns count. */
uint32_t ring_buf_write(ring_buf_t *rb, const void *src, uint32_t count)
{
    uint32_t room  = rb->capacity - rb->write_ptr;
    uint32_t first = (count < room) ? count : room;   /* min(count, cap-write) */
    const uint8_t *s = (const uint8_t *)src;

    if (rb->capacity < rb->write_ptr + first) {       /* bounds trap */
        port_set_interrupt_mask();
        for (;;) { }
    }
    mem_cpy(rb->data_buf + rb->write_ptr, s, first);

    if (first < count) {                               /* wrap */
        if ((count - first) > rb->capacity) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        mem_cpy(rb->data_buf, s + first, count - first);
    }

    rb->write_ptr += count;
    if (rb->write_ptr >= rb->capacity)
        rb->write_ptr -= rb->capacity;
    return count;
}

/* ------------------------------------------------------------------- 0x56dc */

/* write up to `want` bytes capped by free space. In length-prefix mode (flags
 * bit0) a single 4-byte u32 length header is prepended, and the payload is only
 * written when free_bytes >= min_required. OEM 0x56dc. */
uint32_t ring_buf_write_capped(ring_buf_t *rb, const void *src, uint32_t want,
                               uint32_t free_bytes, uint32_t min_required)
{
    uint32_t n = want;

    if (rb->flags & 0x01u) {
        /* length-prefix framing: header (4 bytes) + payload, all-or-nothing */
        if (free_bytes < min_required)
            return 0;
        ring_buf_write(rb, &n, 4);            /* the 4-byte u32 length header */
    } else {
        /* plain mode: silently cap to available space */
        if (n > free_bytes)
            n = free_bytes;
    }

    if (n == 0) {
        port_set_interrupt_mask();            /* zero-length write trap */
        for (;;) { }
    }
    return ring_buf_write(rb, src, n);
}

/* ------------------------------------------------------------------- 0x59d8 */

/* top-level write + post-write consumer notify. Thread context uses a FreeRTOS
 * critical section and the TX-complete callback; ISR context masks BASEPRI, uses
 * the RX-complete callback and pends PendSV. OEM 0x59d8.
 *
 * NOTE (verifier): the return is INVERTED — true means the write did NOT place
 * all `count` bytes (partial/failed); false means full success. */
bool ring_buf_write_notify(ring_buf_t *rb, const void *src, uint32_t count, int *pend_sv)
{
    /* min_required: length-prefix mode needs count+4; else min(capacity-1, count) */
    uint32_t min_req = (rb->flags & 0x01u)
                         ? count + 4u
                         : ((rb->capacity - 1u < count) ? rb->capacity - 1u : count);
    uint32_t written;
    void    *sess;
    (void)pend_sv;   /* OEM never reads param_4 — ISR uses a stack-local woken flag */

    if (__get_ipsr() == 0) {
        /* thread context */
        if (src == NULL) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        vTaskEnterCritical();
        written = ring_buf_write_capped(rb, src, count, ring_buf_bytes_free(rb), min_req);
        if (ring_buf_bytes_used(rb) >= rb->notify_thresh) {
            prvIncrementSuspendedCounter();
            sess = rb->session;
            if (sess != NULL) {
                can_session_tx_complete((can_session_node_t *)sess);
                rb->session = NULL;
            }
            xTimerGenericCommand(sess);          /* unconditional once threshold met */
        }
        vTaskExitCritical();
    } else {
        /* ISR context */
        uint32_t woken = 0;
        uint32_t mask;
        if (src == NULL || rb == NULL) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        mask = port_set_interrupt_mask();
        written = ring_buf_write_capped(rb, src, count, ring_buf_bytes_free(rb), min_req);
        if (ring_buf_bytes_used(rb) >= rb->notify_thresh) {
            sess = rb->session;
            if (sess != NULL) {
                can_session_rx_complete((can_session_node_t *)sess, &woken);
                rb->session = NULL;
            }
        }
        port_clear_interrupt_mask(mask);
        if (woken)
            MMIO32(SCB_ICSR) = SCB_ICSR_PENDSVSET;
    }

    return written != count;                  /* true = partial / failed */
}
