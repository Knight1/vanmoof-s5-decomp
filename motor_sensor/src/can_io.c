/*
 * can_io.c — VanMoof S5 motor_sensor: CAN frame I/O core.
 *
 * The submit/receive/notify plumbing under the report frames: TX submit, RX
 * dispatch, the 13-byte subscriber-notification frame builder, the FD/classical
 * RX vtable dispatcher, the flash-backed log writer, and the watermark flow
 * control. Translated from the OEM image (LPC546xx-class, base 0x0); FreeRTOS +
 * memcpy + the vtable driver are vendor.
 *
 * OEM: can_tx_submit_frame 0x59fa, can_tx_buf_write 0x5a70,
 * can_frame_receive_and_dispatch 0x599a, can_frame_notify_subscribers 0x5914,
 * can_rx_enqueue 0x0a04, can_tx_send_with_flow_control 0x21a8.
 */

#include "motor_sensor.h"

extern can_ctrl_state_t *g_can_ctrl;     /* DAT_0a24/0760 = 0x1301FE00 */
extern void            **g_active_conn;  /* 0x002000FC -> active connection */

/* ------------------------------------------------------------------- 0x59fa */

/* Submit a TX frame for CAN-ID 0x008808: lookup slot, take sem (100 ticks),
 * copy 8 bytes, re-resolve, notify + wake the TX task. Returns 0 / -1. */
uint32_t can_tx_submit_frame(void *ctx, uint32_t *frame, uint32_t p3)
{
    uint8_t key[4] = { 0x08, 0x88, 0x00, 0 };          /* CAN ID 0x008808 */
    sub_entry_t *e;
    (void)p3;

    e = (sub_entry_t *)(intptr_t)subscription_find_entry_or_null(ctx, key, NULL, NULL, NULL);
    if (e == NULL)
        return 0xffffffffu;
    /* acquire e->sem (100-tick timeout) */
    mem_cpy(e->frame_buf, frame, 8);
    e = (sub_entry_t *)(intptr_t)subscription_find_entry_or_null(ctx, key,
            (void *)(uintptr_t)frame[1], NULL, ctx);
    if (e == NULL)
        return 0xffffffffu;
    can_frame_notify_subscribers(ctx, e);
    freertos_timer_generic_command((void *)(uintptr_t)e->sem);
    return 0;
}

/* ------------------------------------------------------------------- 0x5a70 */

/* Flash-backed log writer (named for its CAN-TX caller). Appends (src,len) at
 * page_count*sector_size + write_pos via flash_write_sector. Returns 0 / -1. */
uint32_t can_tx_buf_write(void *obj_, uint32_t p2, const void *src, int len)
{
    uint8_t  *obj = (uint8_t *)obj_;
    uint16_t  sector_size = *(uint16_t *)(obj + 0x06);
    uint16_t  write_pos   = *(uint16_t *)(obj + 0x08);
    uint16_t  page_count  = *(uint16_t *)(obj + 0x0c);
    void     *flash       = *(void **)(obj + 0x18);

    *(uint32_t *)(obj + 0x10) = p2;                    /* sequence/tag */
    obj[0x15] = (uint8_t)p2;
    if (sector_size == 0xffffu)                        /* uninitialised */
        return 0xffffffffu;
    if ((uint32_t)write_pos + (uint32_t)len > page_count)
        return 0xffffffffu;

    if (flash_write_sector((flash_obj_t *)flash,
                           (uint32_t)page_count * sector_size + write_pos, src, (uint32_t)len) != 0)
        return 0xffffffffu;
    *(uint16_t *)(obj + 0x08) = (uint16_t)(write_pos + len);
    return 0;
}

/* ------------------------------------------------------------------- 0x599a */

/* RX dispatch: resolve the entry, require active (state==2), take the sem
 * (infinite), optionally run the per-entry rx callback (a -3 return vetoes),
 * fan out via can_frame_notify_subscribers, then release. */
void can_frame_receive_and_dispatch(void *ctx, uint8_t *frame_id_block)
{
    sub_entry_t *e = (sub_entry_t *)(intptr_t)
        subscription_find_entry_or_null(ctx, frame_id_block, NULL, NULL, NULL);
    if (e == NULL || e->state != 0x02)
        return;
    /* take e->sem with portMAX_DELAY (0x7fffffff) */
    if (frame_id_block[3] != 0 && e->rx_cb != NULL) {
        ms_rx_cb_fn cb = (ms_rx_cb_fn)e->rx_cb;
        if (cb(e->notify_q, &e->frame_inline, e->frame_buf, e->aux) == -3)
            { freertos_queue_send_from_isr_or_task((void *)(uintptr_t)e->sem); return; }
    }
    can_frame_notify_subscribers(ctx, e);
    freertos_queue_send_from_isr_or_task((void *)(uintptr_t)e->sem);  /* release */
}

/* ------------------------------------------------------------------- 0x5914 */

/* Build a 13-byte notification frame from a CAN frame descriptor and fan it out
 * to subscribers. Type 0 (data) inlines ≤8 payload bytes + sets flag 0x10 and
 * calls the dispatch fn at ctx+0x5a4; types 1/2 stream via chunk writer (type 1
 * sets the flag, type 2 does not). Returns -2 / -1 / 0. */
uint32_t can_frame_notify_subscribers(void *ctx_, void *frame_desc_)
{
    uint8_t *ctx   = (uint8_t *)ctx_;
    uint8_t *fd    = (uint8_t *)frame_desc_;
    uint8_t  notif[MS_FRAME_LEN];
    uint32_t id;
    uint8_t  type;

    if (ctx_ == NULL || frame_desc_ == NULL)
        return 0xfffffffeu;

    id   = *(uint32_t *)(fd + 0x0c);
    type = fd[0x10];
    mem_set(notif, 0, sizeof notif);
    notif[0] = (uint8_t)id;
    notif[1] = (uint8_t)(id >> 8);
    notif[2] = (uint8_t)(id >> 16);

    switch (type) {
    case 0: {                                          /* data frame */
        uint32_t plen = *(uint32_t *)(fd + 0x24);
        void    *pdat = *(void **)(fd + 0x20);
        void   (*dispatch)(void *, void *) = *(void (**)(void *, void *))(ctx + 0x5a4);
        if (plen > 8u) plen = 8u;
        notif[4] = (uint8_t)plen;
        mem_cpy(&notif[5], pdat, plen);
        notif[3] |= MS_FRAME_FLAG;
        dispatch(ctx + 0x594, notif);                  /* subscriber list @+0x594 */
        return 0;
    }
    case 1:                                            /* TX request */
        notif[3] |= MS_FRAME_FLAG;
        can_tx_frame_write_chunks(ctx + 0x594, fd);
        return 0;
    case 2:                                            /* TX confirm (no flag) */
        can_tx_frame_write_chunks(ctx + 0x594, fd);
        return 0;
    default:
        return 0xffffffffu;
    }
}

/* ------------------------------------------------------------------- 0x0a04 */

/* RX enqueue: dispatch the incoming frame to the FD or classical vtable write
 * function depending on the controller mode (tail-call in the OEM). */
void can_rx_enqueue(void *rx_frame)
{
    vfn_t *vtable = g_can_ctrl->vtable;                /* *(*0x1301FE00 + 0x10) */
    if (can_is_fd_mode(g_can_ctrl)) {
        ms_rx_write_fn fd_rx = (ms_rx_write_fn)vtable[0x30 / 4];
        fd_rx(rx_frame, 0, 0);
    } else {
        ms_rx_write_fn classical_rx = (ms_rx_write_fn)vtable[0x24 / 4];
        classical_rx(rx_frame, 0, 0);
    }
}

/* ------------------------------------------------------------------- 0x21a8 */

/* Watermark-based TX flow control (task + ISR contexts). Sends only when the
 * ring occupancy exceeds the watermark (0 classical / 4 FD per conn flags bit0);
 * drains up to the watermark (capped 0x8c, restoring the read head on overflow)
 * then sends the payload. Returns bytes sent, or 0 below the watermark. */
int can_tx_send_with_flow_control(uint32_t *conn_, uint32_t param2, int is_isr)
{
    uint8_t  *conn = (uint8_t *)conn_;
    uint32_t  watermark = (conn[0x1c] & 0x01u) ? 4u : 0u;
    ring_buf_t *rb = (ring_buf_t *)conn;
    uint32_t  avail, used, saved_read;
    uint8_t   drain[0x8c];

    if (is_isr != 0) {                                 /* task path */
        vTaskExitCritical();
        void *cobj = (g_active_conn ? *g_active_conn : NULL);
        if (cobj && *(uint8_t *)((uint8_t *)cobj + 0x68) == 0x02)
            *(uint8_t *)((uint8_t *)cobj + 0x68) = 0;
        if (*(uint32_t *)(conn + 0x10) != 0) { /* assert pending_ptr == 0 */ }
        *(uint32_t *)(conn + 0x10) = (uint32_t)(uintptr_t)conn;
    }

    used = (rb->write_ptr + rb->capacity - rb->read_ptr) % rb->capacity;
    if (used <= watermark)
        return 0;                                      /* below watermark */

    saved_read = rb->read_ptr;
    avail = used - watermark;
    if (avail > sizeof drain) avail = sizeof drain;
    ringbuf_read_consume(rb, drain, avail, watermark); /* drain to watermark */
    if (rb->read_ptr < saved_read) rb->read_ptr = saved_read;   /* overflow guard */

    {
        uint8_t payload[0x8c];
        uint32_t n = ringbuf_read_consume(rb, payload, used - watermark, (uint32_t)param2);
        if (*(uint32_t *)(conn + 0x14) != 0) {         /* abort pending connection */
            *(uint32_t *)(conn + 0x14) = 0;
        }
        return (int)n;
    }
}
