/*
 * can_report.c — VanMoof S5 motor_sensor: outbound CAN report frames.
 *
 * The sensor board's CAN messages to the fleet: the high-bandwidth sensor data
 * frame (0x8887), the status frames (0x0382, 0x02A1), the brake-status frame
 * (0x04A1), the low-level 29-bit-ID packer, and the diagnostic logger. All but
 * the packer share the subscription-slot + semaphore + notify-subscribers
 * pattern. Translated from the OEM image (LPC546xx-class, base 0x0); the slot
 * lookup, semaphore and stream-buffer primitives are vendor.
 *
 * OEM: can_send_status_frame_382 0x2580, can_send_data_frame_8887 0x25ee,
 * can_send_brake_status_4a1 0x2dd4, can_send_status_frame_2a1 0x26bc,
 * can_frame_pack_and_enqueue 0x544a, can_diag_log_send 0x2e80.
 */

#include "motor_sensor.h"
#include <stdarg.h>

extern const uint8_t      g_status382_payload[8];  /* @0x6d9c */
extern volatile uint32_t  g_status2a1_payload;     /* @0x200000e4 (the word itself) */
extern void             **g_diag_endpoint_ptr;     /* @0x200001bc -> diag obj */
extern uint32_t           g_diag_channel;          /* 0x000031ed */

/* lookup a TX subscription slot by 3-byte CAN-ID key (vendor helper @0x4d3e). */
static sub_entry_t *find_slot(void *ctx, uint8_t id0, uint8_t id1, uint8_t id2)
{
    uint8_t key[4] = { id0, id1, id2, 0 };
    return (sub_entry_t *)(intptr_t)subscription_find_entry_or_null(ctx, key, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------- 0x2580 */

/* CAN 0x0382: fixed 8-byte status payload from g_status382_payload. */
void can_send_status_frame_382(uint32_t *ctx, uint32_t a, uint32_t b)
{
    sub_entry_t *e = find_slot(ctx, 0x82, 0x03, 0x00);
    (void)a; (void)b;
    if (e == NULL)
        return;
    /* acquire the TX slot semaphore (100-tick timeout) then publish */
    mem_cpy(e->frame_buf, g_status382_payload, 8);
    e = find_slot(ctx, 0x82, 0x03, 0x00);              /* re-resolve */
    if (e == NULL)
        return;
    can_frame_notify_subscribers(ctx, e);
    freertos_timer_generic_command((void *)(uintptr_t)e->sem);
}

/* ------------------------------------------------------------------- 0x25ee */

/* CAN 0x8887: 30-byte sensor data payload (rotor angle / current / temp). */
void can_send_data_frame_8887(uint32_t a, uint32_t *data, uint32_t c, uint32_t **ctx)
{
    sub_entry_t *e;
    (void)a; (void)c;
    if (ctx == NULL || *ctx == NULL)
        return;
    e = find_slot(ctx, 0x87, 0x88, 0x00);
    if (e == NULL)
        return;
    /* bulk-copy 30 bytes (7 words + 1 halfword) into the frame buffer */
    mem_cpy(e->frame_buf, data, 30);
    e = find_slot(ctx, 0x87, 0x88, 0x00);
    if (e == NULL)
        return;
    can_frame_notify_subscribers(ctx, e);
    freertos_timer_generic_command((void *)(uintptr_t)e->sem);
}

/* ------------------------------------------------------------------- 0x2dd4 */

/* CAN 0x04A1: brake-engaged bit (from *brake bit0), mirrored to MMIO
 * 0x4008c000+9; on first call starts a 1 s re-send timer. */
uint32_t can_send_brake_status_4a1(uint32_t a, uint32_t b, uint8_t *brake, uint32_t d)
{
    sub_entry_t *e = find_slot((void *)(uintptr_t)a, 0xa1, 0x04, 0x00);
    uint8_t bit;
    (void)b; (void)d;
    if (e == NULL)
        return 0xffffffffu;

    if (e->timer == NULL) {                            /* lazy 1 s one-shot timer */
        void *blk = pvPortMalloc(0x0c);
        if (blk == NULL)
            return 0xffffffffu;
        e->timer = xTimerCreate_wrap(blk, 0x3e8, 0, (void *)(uintptr_t)a, NULL);
    }

    bit = *brake & 0x01u;
    MMIO32(0x4008c000u + 9) = bit;                     /* MMIO shadow */
    /* BFI bit0 into frame_buf byte[0] without disturbing other bits */
    ((uint8_t *)e->frame_buf)[0] = (uint8_t)((((uint8_t *)e->frame_buf)[0] & ~1u) | bit);

    e = find_slot((void *)(uintptr_t)a, 0xa1, 0x04, 0x00);
    if (e == NULL)
        return 0xffffffffu;
    can_frame_notify_subscribers((void *)(uintptr_t)a, e);
    return 0;
}

/* ------------------------------------------------------------------- 0x26bc */

/* CAN 0x02A1: single 4-byte status word from g_status2a1_payload. */
void can_send_status_frame_2a1(uint32_t a, uint32_t b, uint32_t c)
{
    sub_entry_t *e = find_slot((void *)(uintptr_t)a, 0xa1, 0x02, 0x00);
    uint32_t word = g_status2a1_payload;               /* the word, not a ptr */
    (void)b; (void)c;
    if (e == NULL)
        return;
    *(uint32_t *)e->frame_buf = word;
    e = find_slot((void *)(uintptr_t)a, 0xa1, 0x02, 0x00);
    if (e == NULL)
        return;
    can_frame_notify_subscribers((void *)(uintptr_t)a, e);
    freertos_timer_generic_command((void *)(uintptr_t)e->sem);
}

/* ------------------------------------------------------------------- 0x544a */

/* Pack a raw {4-byte-id, dlc, payload} descriptor into a 16-byte queue item and
 * enqueue (non-blocking). The 29-bit extended ID is assembled big-endian from
 * four byte fields. Returns 0 / -1. */
uint32_t can_frame_pack_and_enqueue(void *ctx, uint8_t *raw)
{
    uint8_t  frame[16];
    uint32_t id;
    uint8_t  dlc;
    void    *queue = *(void **)((uint8_t *)ctx + 0x14);

    id  = ((uint32_t)raw[0] << 21) | ((uint32_t)raw[1] << 13) |
          ((uint32_t)raw[2] << 5)  | (uint32_t)(raw[3] & 0x1fu);
    dlc = raw[4] > 8u ? 8u : raw[4];

    mem_set(frame, 0, sizeof frame);
    if (queue == NULL)
        return 0xffffffffu;
    frame[0] = dlc;
    mem_cpy(&frame[8], raw + 5, 11);                   /* payload */
    *(uint32_t *)&frame[4] = id;

    return queue_send_item(queue, frame, 0) == 1 ? 0u : 0xffffffffu;
}

/* ------------------------------------------------------------------- 0x2e80 */

/* Variadic diagnostic logger: 30-byte packet (opcode@0, event@4, args@0x1a)
 * sent over the diagnostic stream buffer (handle at diag-obj+0x590). */
void can_diag_log_send(uint32_t opcode, uint16_t event, int argc, ...)
{
    uint8_t  pkt[0x1e];
    void    *obj = (g_diag_endpoint_ptr ? *g_diag_endpoint_ptr : NULL);
    va_list  ap;
    int      i;

    if (obj == NULL)
        return;                                        /* endpoint not registered */

    mem_set(pkt, 0, sizeof pkt);
    *(uint32_t *)(pkt + 0) = opcode;
    *(uint16_t *)(pkt + 4) = event;
    va_start(ap, argc);
    for (i = 0; i < argc && (size_t)(0x1a + i * 4) < sizeof pkt; i++)
        *(uint32_t *)(pkt + 0x1a + i * 4) = va_arg(ap, uint32_t);
    va_end(ap);

    freertos_stream_buffer_send_blocking(g_diag_channel,
                                         pkt, sizeof pkt, 0);
    (void)obj;
}
