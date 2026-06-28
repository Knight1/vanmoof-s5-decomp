/*
 * can_session.c — VanMoof S5 imx8_bridge CAN-TP session layer.
 *
 * Sessions are the transport that carries a multi-frame CAN-FD exchange between
 * an ECU and the i.MX8 host: open/close, write (with optional 4-byte CAN-TP
 * header), the RX/TX completion callbacks that file a finished session into the
 * priority-indexed done-table, and the round-robin scheduler that picks the next
 * active session. Translated from the OEM image (NXP LPC55S69, base 0x0);
 * FreeRTOS list/critical primitives are vendor.
 *
 * OEM: can_session_open 0x35d4, can_session_write 0x33ec,
 * can_session_rx_complete 0x3244, can_session_tx_complete 0x32d8,
 * can_session_table_advance 0x3a6c.
 */

#include "imx8_bridge.h"

/* RAM globals reached via literal pools (addresses are the OEM RAM targets). */
extern void              *g_spi_chan_open;     /* *DAT_36b0  — SPI channel handle for open */
extern void             **g_active_spi_chan;   /* DAT_34cc   — ptr-to-ptr active SPI channel */
extern volatile uint32_t  g_sess_mode_flag;    /* *DAT_32c0  -> 0x200009a4 (0=normal) */
extern volatile uint32_t  g_sess_rx_watermark; /* *DAT_32c4  -> 0x200009ac */
extern uint8_t           *g_sess_rx_table;     /* *DAT_32c8  -> 0x2000075c (0x14-stride) */
extern void              *g_sess_rx_overflow;  /* *DAT_32d4  -> 0x20000fe4 */
extern can_session_node_t**g_sess_rx_best;     /* *DAT_32cc  -> 0x20000744 */
extern volatile uint32_t  g_sess_rx_wake;      /* *DAT_32d0  -> 0x20001078 */
extern volatile uint32_t  g_sess_tx_watermark; /* *DAT_333c  -> 0x200009ac */
extern uint8_t           *g_sess_tx_table;     /* *DAT_3340  -> 0x2000075c */
extern can_session_node_t**g_sess_tx_best;     /* *DAT_3344  -> 0x20000744 */
extern uint8_t           *g_sess_sched_table;  /* *DAT_3ad0  -> 0x2000075c (0x14-stride) */
extern volatile uint32_t  g_sched_lock;        /* *DAT_3ac4  -> 0x200009a4 */
extern volatile uint32_t  g_sched_pending;     /* *DAT_3ac8  -> 0x20001078 */
extern volatile uint32_t  g_sched_index;       /* *DAT_3acc  -> 0x200009ac */
extern volatile uint32_t  g_sched_peer;        /* *DAT_3ad4  -> 0x20000744 */

#define SESS_TIMER_CFG  0x5cfbu        /* DAT_36ac */
#define SESS_STRIDE     0x14u          /* done-table stride */

/* ------------------------------------------------------------------- 0x35d4 */

/* Open (or re-open) a CAN-TP session. Closes any existing one, allocates the
 * 0x28-byte session object, arms its timer, and posts the open command (cmd=1 in
 * task context, cmd=6 FromISR). Returns false on a failed queue send. */
bool can_session_open(void **handle, int timeout_ms, int bidirectional, int callback)
{
    can_session_t *obj;
    uint32_t ticks;
    int rc;

    if (handle == NULL)                        /* OEM: NULL handle = no-op, returns true */
        return true;

    if (*handle != NULL) {                     /* close the previous session */
        spi_queue_send(*handle, SPI_CMD_CLOSE, 0, NULL, 0, 0, 0);
        *handle = NULL;
    }

    /* ms -> ticks (1000-tick/s passthrough); INT32_MAX -> infinite */
    ticks = (timeout_ms == 0x7fffffff) ? 0xffffffffu
                                       : (uint32_t)(((uint32_t)timeout_ms * 1000u) / 1000u);
    if (ticks == 0) {                          /* configASSERT: non-zero timeout */
        port_set_interrupt_mask();
        for (;;) { }
    }

    obj = (can_session_t *)heap_malloc(0x28);
    if (obj == NULL) {                         /* OEM: alloc failure returns true */
        *handle = NULL;
        return true;
    }

    obj->flags         = 0;                    /* +0x24 zeroed before timer_init */
    obj->seq           = 0;                    /* +0x00 */
    obj->flags2        = 0;                    /* +0x14 */
    obj->timeout_ticks = ticks;                /* +0x18 */
    obj->back_ptr      = handle;               /* +0x1c back-pointer (verifier) */
    obj->timer_cfg     = SESS_TIMER_CFG;       /* +0x20 */
    if (bidirectional == 0)
        obj->flags |= 0x04;                    /* +0x24 bit2: unidirectional */

    *handle = obj;
    ((int *)handle)[1] = callback;             /* session_handle[1] = callback */
    ((int *)handle)[2] = 0;                    /* session_handle[2] = 0 */

    can_session_timer_init();

    if (__get_ipsr() == 0) {                   /* task context */
        rc = (int)spi_queue_send(*handle, SPI_CMD_OPEN, (uint32_t)(uintptr_t)g_spi_chan_open,
                                 NULL, 0xffffffffu, 0, 0);
    } else {                                   /* ISR context */
        int woken = 0;
        rc = (int)spi_queue_send(*handle, SPI_CMD_OPEN_ISR, (uint32_t)(uintptr_t)g_spi_chan_open,
                                 &woken, 0, 0, 0);
        if (woken)
            MMIO32(SCB_ICSR) = SCB_ICSR_PENDSVSET;
    }
    return rc == 0;
}

/* ------------------------------------------------------------------- 0x33ec */

/* Write a CAN-TP session's payload out toward the SPI side. Optionally prepends
 * a 4-byte CAN-TP header (flags bit0). The cookie path flushes an in-progress
 * SPI transfer before resuming. OEM asserts buf!=NULL and session!=NULL. */
int can_session_write(ring_buf_t *session, uint8_t *buf, uint32_t len, int cookie)
{
    uint8_t *sbytes = (uint8_t *)session;
    uint32_t hdr = (session->flags & 0x01u) ? 4u : 0u;   /* +0x1c bit0 */
    uint32_t got;

    if (buf == NULL || session == NULL) {      /* configASSERT (verifier) */
        port_set_interrupt_mask();
        for (;;) { }
    }

    if (cookie != 0) {
        vTaskEnterCritical();                              /* outer critical */
        if (ring_buf_bytes_used(session) <= hdr) {
            void *chan = *g_active_spi_chan;
            vTaskEnterCritical();                          /* inner critical */
            if (*(uint8_t *)((uint8_t *)chan + 0x68) == 2) /* clear only if busy */
                *(uint8_t *)((uint8_t *)chan + 0x68) = 0;
            if (*(void **)(sbytes + 0x10) != NULL) {       /* assert wait-slot empty */
                port_set_interrupt_mask();
                for (;;) { }
            }
            *(void **)(sbytes + 0x10) = chan;              /* park channel in wait slot */
            vTaskExitCritical();                           /* exit inner */
            vTaskExitCritical();                           /* exit outer */
            spi_transfer_complete_handler(cookie);
            *(void **)(sbytes + 0x10) = NULL;
        } else {
            vTaskExitCritical();
        }
    }

    if (ring_buf_bytes_used(session) <= hdr)
        return 0;                              /* nothing to send */

    if (hdr != 0) {
        uint8_t  hbuf[4];
        uint32_t saved_rd = session->read_ptr;         /* seq/head snapshot */
        ring_buf_read(session, hbuf, hdr, ring_buf_bytes_used(session));
        if (len < hdr) {
            len = 0;
            session->read_ptr = saved_rd;              /* restore on underflow */
        } else {
            len = hdr;
        }
    }

    got = ring_buf_read(session, buf, len, ring_buf_bytes_used(session));
    if (got != 0) {
        prvIncrementSuspendedCounter();
        if (*(void **)(sbytes + 0x14) != NULL) {             /* +0x14 chained tx */
            can_session_tx_complete(*(can_session_node_t **)(sbytes + 0x14));
            *(void **)(sbytes + 0x14) = NULL;                /* OEM clears it after */
        }
        xTimerGenericCommand(session);
    }
    return (int)got;
}

/* ------------------------------------------------------------------- 0x3244 */

/* RX-complete callback: mark the session done and file it into the priority-
 * indexed RX done-table; wake the consumer if this is the new best session.
 * IRQ-safe (port mask). */
void can_session_rx_complete(can_session_node_t *s, uint32_t *wake_out)
{
    uint32_t mask;
    uint8_t prev;

    if (s == NULL) {
        port_set_interrupt_mask();
        for (;;) { }
    }
    mask = port_set_interrupt_mask();
    prev = s->state;                           /* +0x68 */
    s->state = 2;                              /* done */

    if (prev == 1) {
        if (s->error_flag != 0) {              /* +0x28 must be 0 */
            port_set_interrupt_mask();
            for (;;) { }
        }
        if (g_sess_mode_flag == 0) {           /* normal mode */
            void *node = uxListRemove(&s->list_node);          /* +0x04 */
            if (s->count > g_sess_rx_watermark)                /* +0x2c */
                g_sess_rx_watermark = s->count;
            vListInsertEnd(g_sess_rx_table + s->count * SESS_STRIDE, node);
        } else {                               /* overflow mode */
            vListInsertEnd(g_sess_rx_overflow, &s->alt_list_node);  /* +0x18 */
        }
        if (s->count > (*g_sess_rx_best)->count) {   /* OEM writes *wake_out unconditionally */
            *wake_out = 1;
            g_sess_rx_wake = 1;
        }
    }
    port_clear_interrupt_mask(mask);
}

/* ------------------------------------------------------------------- 0x32d8 */

/* TX-complete callback: symmetric to rx_complete but for the TX done-table; pends
 * a context switch if this session outranks the current best, then notifies the
 * TX consumer task. */
void can_session_tx_complete(can_session_node_t *s)
{
    uint8_t prev;

    if (s == NULL) {
        port_set_interrupt_mask();
        for (;;) { }
    }
    vTaskEnterCritical();
    prev = s->state;
    s->state = 2;

    if (prev == 1) {
        void *node = uxListRemove(&s->list_node);
        if (s->count > g_sess_tx_watermark)
            g_sess_tx_watermark = s->count;
        vListInsertEnd(g_sess_tx_table + s->count * SESS_STRIDE, node);
        if (s->error_flag != 0) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        if (s->count > (*g_sess_tx_best)->count)
            port_yield_pend_sv();
    }
    vTaskExitCritical();                       /* tail: FromISR notify (0xe04) */
}

/* ------------------------------------------------------------------- 0x3a6c */

/* Round-robin advance of the active session index over the 0x14-stride table.
 * Defers if the table is locked. Walks backward to the first occupied slot,
 * rotates its circular list one node, and publishes the new peer handle. */
void can_session_table_advance(void)
{
    uint8_t *base = g_sess_sched_table;
    uint32_t idx;

    if (g_sched_lock != 0) {                   /* table busy */
        g_sched_pending = 1;
        return;
    }
    g_sched_pending = 0;
    idx = g_sched_index;

    for (;;) {
        uint8_t *slot = base + idx * SESS_STRIDE;
        if (*(uint32_t *)slot != 0) {          /* slot occupied */
            uint8_t **fwd  = (uint8_t **)(slot + 4);          /* +0x04 list head */
            uint8_t  *next = *(uint8_t **)(*fwd + 4);         /* head->next */
            if (next == slot + 8)                              /* skip this slot's sentinel */
                next = *(uint8_t **)(next + 4);                /* sentinel->next */
            *fwd = next;                                       /* update head */
            g_sched_peer = *(uint32_t *)(*fwd + 0x0c);         /* peer handle via updated head */
            g_sched_index = idx;
            return;
        }
        if (idx == 0) {                        /* no occupied slot: assert */
            port_set_interrupt_mask();
            for (;;) { }
        }
        idx--;
    }
}
