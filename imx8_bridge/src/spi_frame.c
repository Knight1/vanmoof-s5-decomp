/*
 * spi_frame.c — VanMoof S5 imx8_bridge SPI-slave wire framing.
 *
 * The frame format the bridge speaks to the i.MX8 host over SPI: a 13-byte
 * frame = 3-byte session id, a flags+sequence byte, a length byte, and up to 8
 * payload bytes. Long payloads are streamed as ≤8-byte chunks with a wrapping
 * sequence counter. Translated from the OEM image (NXP LPC55S69, base 0x0);
 * FreeRTOS + memcpy are vendor.
 *
 * OEM addresses: spi_tx_frame_send 0x55f0, spi_frame_write_chunked 0x526a,
 * spi_tx_enqueue 0x584a, spi_channel_alloc 0x5cc0, spi_channel_create 0x5d30,
 * spi_notify_host 0x30b8, spi_queue_send 0x2d6c.
 */

#include "imx8_bridge.h"

/* RAM globals the OEM reaches through literal-pool pointers. */
extern void           *g_spi_tx_queue;       /* DAT_00002db4 — SPI TX queue handle */
extern volatile uint32_t g_spi_notify_token; /* *(DAT_00003104) — host notify token */

/* ------------------------------------------------------------------- 0x55f0 */

/* Build a 13-byte SPI frame from a session and dispatch it. Mode 0 fills the
 * inline payload and calls the single-shot send fn; modes 1/2 stream via the
 * chunked writer. Returns 0, -1 (bad mode) or -2 (null arg). */
int spi_tx_frame_send(spi_obj_t *spi_obj, spi_session_t *sess)
{
    uint8_t  frame[SPI_FRAME_LEN];
    uint32_t id;

    if (spi_obj == NULL || sess == NULL)
        return -2;

    mem_set(frame, 0, SPI_FRAME_LEN);

    id = sess->session_id;                 /* +0x0c: 24-bit session id */
    frame[0] = (uint8_t)id;
    frame[1] = (uint8_t)(id >> 8);
    frame[2] = (uint8_t)(id >> 16);

    switch (sess->mode) {                  /* +0x10 */
    case 1:
        frame[3] |= SPI_FRAME_FLAG;
        spi_frame_write_chunked(spi_obj->ring, sess, frame, 1);
        return 0;
    case 2:
        spi_frame_write_chunked(spi_obj->ring, sess, frame, 1);
        return 0;
    case 0: {
        uint32_t payload_len = sess->payload_len;   /* +0x24 (u32) */
        if (payload_len != 0) {
            frame[4] = (uint8_t)payload_len;        /* length byte (truncated) */
            /* memcpy length is the full u32 word from +0x24 (verifier note) */
            mem_cpy(&frame[5], sess->payload, payload_len);
        }
        frame[3] |= SPI_FRAME_FLAG;
        spi_obj->send_fn(spi_obj->ring, frame);     /* fn ptr @ +0x5a4 */
        return 0;
    }
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------- 0x526a */

/* Stream a session's payload as ≤8-byte chunks, advancing the frame[3] sequence
 * counter (mod-8, or mod-16 for mode-2 sessions). When resume==0 the remaining
 * count starts at 0, so exactly one empty/header-only chunk is emitted
 * (verifier note); otherwise it starts at sess->total_bytes. */
void spi_frame_write_chunked(void *ring, spi_session_t *sess, uint8_t *frame, uint32_t resume)
{
    void (*write_fn)(void *, void *) = *(void (**)(void *, void *))((uint8_t *)ring + 0x10);
    uint32_t remaining = (resume == 0) ? 0 : sess->total_bytes;   /* +0x08 */
    uint32_t offset = 0;

    do {
        uint32_t chunk = (remaining > SPI_CHUNK_MAX) ? SPI_CHUNK_MAX : remaining;
        frame[4] = (uint8_t)chunk;
        if (remaining != 0)
            mem_cpy(&frame[5], sess->data_ptr + offset, chunk);   /* +0x00 base */

        remaining -= frame[4];
        offset    += frame[4];

        write_fn(ring, frame);                                    /* vtable @ ring+0x10 */

        if (sess->mode == 2)                                      /* +0x10 */
            frame[3] = (uint8_t)((frame[3] & 0xf0u) | ((frame[3] + 1) & 0x0fu));  /* mod-16 */
        else
            frame[3] = (uint8_t)((frame[3] & 0xf8u) | ((frame[3] + 1) & 0x07u));  /* mod-8 */
    } while (remaining != 0);
}

/* ------------------------------------------------------------------- 0x584a */

/* Thread/ISR-safe SPI TX enqueue. In thread context delegates to the TX loop; in
 * ISR context masks interrupts, advances the channel, and pends PendSV only when
 * the inline enqueue failed but a queue notification fired (verifier-corrected
 * condition: !enqueue_ok && queue_notified). */
void spi_tx_enqueue(spi_channel_t *ch)
{
    uint32_t mask;
    int enqueue_ok = 0;
    int queue_notified = 0;

    if (__get_ipsr() == 0) {                /* thread context */
        spi_tx_send_loop(ch, 0, 0, 0);
        return;
    }

    if (ch == NULL) {                       /* configASSERT: ch != NULL */
        port_set_interrupt_mask();
        for (;;) { }
    }
    if (ch->elem_size != 0) {               /* +0x40: assert no TX in flight */
        port_set_interrupt_mask();
        for (;;) { }
    }
    if (ch->data_ptr == NULL && ch->end_ptr != NULL) {   /* 3rd configASSERT */
        port_set_interrupt_mask();
        for (;;) { }
    }

    mask = port_set_interrupt_mask();
    if (ch->current_count < ch->capacity) { /* only enqueue when there is room */
        ch->current_count++;                /* +0x38 */
        if (ch->tx_seq == 0xff) {           /* +0x45: queue path */
            queue_notified = prvCopyDataToQueue(&ch->rx_list);   /* &(ch+0x24) */
        } else {
            ch->tx_seq++;                   /* simple increment (0x7f traps) */
            enqueue_ok = 1;
        }
    }
    port_clear_interrupt_mask(mask);

    if (!enqueue_ok && queue_notified)
        MMIO32(SCB_ICSR) = SCB_ICSR_PENDSVSET;
}

/* ------------------------------------------------------------------- 0x5cc0 */

/* Allocate a pointer-ring SPI channel: header (0x48) + capacity*elem_size inline
 * ring. Initialises the ring pointers, sequence bytes (0xff) and the TX/RX
 * FreeRTOS lists. */
spi_channel_t *spi_channel_alloc(uint32_t capacity, uint32_t elem_size)
{
    size_t total = (size_t)capacity * elem_size + 0x48u;
    spi_channel_t *ch;

    /* multiplication-overflow guard (OEM udiv verify) — traps, not returns */
    if (elem_size != 0 && (total - 0x48u) / elem_size != capacity) {
        port_set_interrupt_mask();
        for (;;) { }
    }

    ch = (spi_channel_t *)heap_malloc(total);
    if (ch == NULL)
        return NULL;

    vTaskEnterCritical();
    ch->data_ptr      = (elem_size != 0) ? ch->data : (uint8_t *)ch;
    ch->write_ptr     = ch->data_ptr;
    ch->end_ptr       = ch->data_ptr + (size_t)capacity * elem_size;
    ch->last_ptr      = ch->end_ptr - elem_size;
    ch->current_count = 0;
    ch->capacity      = capacity;
    ch->elem_size     = elem_size;
    ch->seq_a         = 0xff;
    ch->tx_seq        = 0xff;
    vListInitialise(ch->tx_list);           /* +0x10 */
    vListInitialise(ch->rx_list);           /* +0x24 */
    vTaskExitCritical();
    return ch;
}

/* ------------------------------------------------------------------- 0x5d30 */

/* Factory for a degenerate (capacity 1, elem_size 0) channel — a pointer-only
 * pipe — then prime the TX path. */
spi_channel_t *spi_channel_create(void)
{
    spi_channel_t *ch = spi_channel_alloc(1, 0);
    if (ch == NULL)
        return NULL;
    ch->data_ptr = NULL;                    /* +0x00 */
    ch->end_ptr  = NULL;                    /* +0x08 */
    ch->last_ptr = NULL;                    /* +0x0c */
    spi_tx_send_loop(ch, 0, 0, 0);
    return ch;
}

/* ------------------------------------------------------------------- 0x30b8 */

/* Notify the i.MX8 host. Task context sends cmd=2 with the queue timeout; ISR
 * context sends cmd=7 (FromISR) and pends PendSV if a task was woken. */
void spi_notify_host(spi_obj_t *spi_obj, uint32_t a, uint32_t b, int c)
{
    (void)c;
    if (__get_ipsr() == 0) {
        void *queue = spi_obj->queue;       /* +0x00 */
        uint32_t timeout;
        if (queue == NULL) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        timeout = *(uint32_t *)((uint8_t *)queue + 0x18);
        spi_queue_send(queue, SPI_CMD_NOTIFY, g_spi_notify_token, NULL, timeout, a, b);
    } else {
        int woken = 0;
        spi_queue_send(spi_obj->queue, SPI_CMD_NOTIFY_ISR, g_spi_notify_token, &woken, 0, a, b);
        if (woken)
            MMIO32(SCB_ICSR) = SCB_ICSR_PENDSVSET;
    }
}

/* ------------------------------------------------------------------- 0x2d6c */

/* Central SPI queue dispatcher. cmd<6 → task path (spi_tx_send_loop, timeout
 * forced to 0 when the scheduler is not yet running); cmd>=6 → ISR path
 * (spi_rx_buf_consume). Returns 0 if the queue is not yet initialised. */
uint32_t spi_queue_send(void *obj, int cmd, uint32_t pa, void *pb, uint32_t pc,
                        uint32_t a5, uint32_t a6)
{
    void *queue;
    struct { int cmd; uint32_t pa; void *obj; } msg;   /* on-stack {cmd, pa, obj} */
    (void)pb; (void)a5; (void)a6;

    if (obj == NULL) {                      /* configASSERT: obj != NULL */
        port_set_interrupt_mask();
        for (;;) { }
    }
    queue = g_spi_tx_queue;                 /* DAT_2db4 */
    if (queue == NULL)
        return 0;

    /* the callee receives a POINTER to this {cmd,pa,obj} triplet, not cmd in r1 */
    msg.cmd = cmd; msg.pa = pa; msg.obj = obj;

    if (cmd <= 5) {                         /* cmp #5 / bgt: task path */
        uint32_t timeout = pc;
        if (xTaskGetSchedulerState2() != 2) /* scheduler not running */
            timeout = 0;
        return spi_tx_send_loop(queue, (int)(intptr_t)&msg, (int)timeout, 0);
    } else {                                /* ISR path */
        int woken = 0;
        return (uint32_t)spi_rx_buf_consume(queue, &msg, &woken);
    }
}
