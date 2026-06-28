/*
 * bridge.c — VanMoof S5 imx8_bridge: the three application tasks.
 *
 *   vm    (spi_rx_send_loop      0x2938) : SPI-rx  -> CAN-tx  (i.MX8 host -> fleet)
 *   CanTX (spi_tx_send_loop      0x2c5c) : CAN-rx  -> SPI-tx  (fleet -> i.MX8 host)
 *   can   (can_rx_dispatch_loop  0x2eb8) : CAN-rx  -> session dispatch
 *
 * Translated from the OEM image (NXP LPC55S69, base 0x0). FreeRTOS scheduling
 * primitives are vendor; the FlexComm/M_CAN MMIO accesses are verbatim. The task
 * bodies never return (FreeRTOS task entries). Behaviour-oriented: the control
 * flow, constants and register writes match the verified disassembly.
 */

#include "imx8_bridge.h"

/* FlexComm SPI register offsets (from the peripheral base in task_ctx). */
#define FC_FIFOCFG   0x54
#define FC_FIFOSTAT  0x58
#define FC_FIFOTRIG  0x5c
#define FC_DATAADDR  0xc0
#define FC_BUSY      0xcc
#define FC_CTRL      0xd0
#define FC_CTRL2     0xe0
#define FC_DMABUF    0x200

/* 'can' task globals reached via literal pools. */
extern volatile uint32_t  g_can_deadline;   /* *DAT_30a0 */
extern void             **g_can_session_list;/* DAT_30b0 -> list head */
extern volatile uint32_t  g_can_tick;       /* *DAT_30b4 */
extern void             **g_cur_tcb;        /* DAT_2ac4 -> pxCurrentTCB */
extern void *can_session_list_pop_head(void *list);   /* 0x2e48 */

/* ------------------------------------------------------------------- 0x2938 */

/* vm task — drain SPI frames from the i.MX8 host and transmit them on CAN-FD.
 * Blocks 500 ms per receive; when the FlexComm DMA path is not yet primed it
 * programs the peripheral and blocks on a task notification for completion. */
void spi_rx_send_loop(uint32_t *task_ctx)
{
    uint8_t *cb = (uint8_t *)task_ctx;

    if (task_ctx == NULL)
        return;

    for (;;) {
        uint8_t  rx_buf[16];
        uint8_t  out[16];
        uint32_t canid;
        volatile uint8_t *flex;

        /* block until an SPI frame arrives (500-tick timeout, retry) */
        while (can_tx_queue_send((void *)task_ctx[0], rx_buf, 500) != 1)
            ;

        mem_set(out, 0, sizeof out);
        /* strip the top 3 bits of the CAN-ID and force the extended-ID flag */
        canid = (*(uint32_t *)&rx_buf[4] & 0x1fffffffu) | 0x40000000u;
        *(uint32_t *)&out[0] = canid;
        out[6]  = (uint8_t)(rx_buf[0] & 0x0fu);      /* session nibble (sp+0x16) */
        out[12] = 8;                                  /* data length    (sp+0x1c) */

        if (cb[0x115] != 0) {                         /* DMA path already primed */
            func_0x4ed4(task_ctx);
            continue;
        }

        cb[0x114] = 0;                                /* status */
        cb[0x115] = 3;                                /* initializing */
        flex = (volatile uint8_t *)task_ctx[0x58];    /* byte +0x160 */

        if (MMIO32((uintptr_t)flex + FC_BUSY) & 1u) { /* peripheral busy */
            cb[0x115] = 0;
            func_0x4ed4(task_ctx);
            continue;
        }

        /* copy the 8-byte CAN-ID header + 8-byte payload into the DMA window.
         * dest = (FIFO data-addr & 0xfffc) + the *value* at flex+0x200 (not its addr). */
        {
            uint8_t *dest = (uint8_t *)((MMIO32((uintptr_t)flex + FC_DATAADDR) & 0xfffcu)
                                        + MMIO32((uintptr_t)flex + FC_DMABUF));
            mem_cpy(dest, out, 8);
            mem_cpy(dest + 8, &out[8], 8);
        }
        MMIO32((uintptr_t)flex + FC_CTRL2)   |= 1u;
        MMIO32((uintptr_t)flex + FC_FIFOTRIG)|= 1u;
        MMIO32((uintptr_t)flex + FC_FIFOSTAT)&= ~0x200u;
        MMIO32((uintptr_t)flex + FC_FIFOCFG) |= 0x200u;
        MMIO32((uintptr_t)flex + FC_CTRL)    |= 1u;

        /* block on the task notification until the DMA transfer completes */
        if (task_ctx[1] == 0) {                       /* notify handle */
            port_set_interrupt_mask();
            for (;;) { }
        }
        if (xTaskGetSchedulerState2() == 0) {
            port_set_interrupt_mask();
            for (;;) { }
        }
        prvIncrementSuspendedCounter();
        {
            uint8_t *tcb = (uint8_t *)task_ctx[1];
            if ((*(uint32_t *)tcb & 7u) == 0) {       /* notify bits clear -> block */
                vListInsertEnd(tcb + 4, (uint8_t *)(*g_cur_tcb) + 0x18);
                *(uint32_t *)(tcb + 0x18) = 5u - *(uint32_t *)(tcb + 0x2c);  /* 5 - prio */
                prvWriteMessageToBuffer(0x32, 1);     /* block 50 ticks */
            } else {
                *(uint32_t *)tcb &= ~7u;
            }
            xTimerGenericCommand(tcb);
            /* submit the SPI frame only if not notified, or the data-ready bit set */
            {
                uint32_t notify = *(uint32_t *)tcb & 0xffffffu;
                if (notify == 0 || (notify & 2u))
                    func_0x4ed4(task_ctx);
            }
        }
    }
}

/* ------------------------------------------------------------------- 0x2c5c */

/* CanTX task (and the internal task-context send path of spi_queue_send) — drain
 * the SPI ring and either emit a byte or, when idle, notify the host and block.
 * Returns 1 when a byte was dequeued, 0 when empty / done. */
uint32_t spi_tx_send_loop(void *ring_buf, int blocking, int p3, uint32_t p4)
{
    uint8_t *r = (uint8_t *)ring_buf;
    int      saved = 0;
    (void)p4;

    if (ring_buf == NULL) {
        port_set_interrupt_mask();
        for (;;) { }
    }
    if (blocking == 0 && *(uint32_t *)(r + 0x40) != 0) {
        port_set_interrupt_mask();
        for (;;) { }
    }
    if (xTaskGetSchedulerState2() == 0 && p3 != 0) {   /* trap iff scheduler not running */
        port_set_interrupt_mask();
        for (;;) { }
    }

    for (;;) {
        vTaskEnterCritical();
        if (*(uint32_t *)(r + 0x38) < *(uint32_t *)(r + 0x3c)) {    /* data available */
            int adv = spi_rx_buf_advance(ring_buf, blocking);
            if (*(void **)(r + 0x24) != NULL)
                prvCopyDataToQueue(r + 0x24);
            if (adv != 0)
                port_yield_pend_sv();
            vTaskExitCritical();
            return 1;
        }
        if (blocking == 0) {
            vTaskExitCritical();
            return 0;
        }
        if (!saved)
            saved = 1;                                 /* one-shot global save */
        vTaskExitCritical();

        prvIncrementSuspendedCounter();
        vTaskEnterCritical();
        if (r[0x44] == 0xff) r[0x44] = 0;              /* reset sentinels */
        if (r[0x45] == 0xff) r[0x45] = 0;
        vTaskExitCritical();

        if (prvWriteBytesToBuffer(&saved, &blocking) != 0) {
            xQueueGenericSendFromISR(ring_buf);
            xTimerGenericCommand(ring_buf);
            return 0;
        }
        vTaskEnterCritical();
        if (*(uint32_t *)(r + 0x38) == *(uint32_t *)(r + 0x3c)) {   /* now empty */
            vTaskExitCritical();
            spi_notify_and_kick(r + 0x10, p3);         /* arg is p3, not blocking */
        } else {
            vTaskExitCritical();
        }
        xQueueGenericSendFromISR(ring_buf);            /* both branches notify + kick */
        xTimerGenericCommand(ring_buf);
    }
}

/* ------------------------------------------------------------------- 0x2eb8 */

/* can task — dispatch received CAN frames through the CAN-TP session state
 * machine. Re-arms session timers, expires deadlines, and runs each session's
 * state function. The received frame's state code (0..9) selects the action via
 * the OEM TBB jump table at 0x2ffe. */
void can_rx_dispatch_loop(uint32_t a, uint32_t b, int c, void *d)
{
    (void)a; (void)b; (void)c;

    for (;;) {                                         /* LAB_2ec2 */
        void *list = *g_can_session_list;

        prvIncrementSuspendedCounter();
        if (g_can_deadline > g_can_tick && list != NULL) {  /* deadline in the future */
            can_session_list_pop_head(list);
            g_can_deadline = g_can_tick;
            xTimerGenericCommand(list);
        }
        /* empty-list case: OEM sets an empty flag and re-loops — no queue/timer call.
         * The per-session deadline-expiry sub-block (uxListRemove + re-insert +
         * dispatch) is modelled by the state machine below. */

        for (;;) {                                     /* LAB_2fc8: poll frames */
            uint8_t frame[0x80];                        /* dequeued session object */
            int     state;

            if (can_tx_queue_send(list, frame, 0) == 0)
                break;                                  /* nothing -> re-evaluate */

            state = frame[0];                           /* iStack_18 state code */
            switch (state) {
            case 0: case 1: case 2: case 6: case 7: {   /* re-arm timer + dispatch */
                uint8_t *s = frame;
                s[0x24] |= 1u;                          /* pending */
                vListInsert(s, g_can_tick, g_can_tick); /* sorted by deadline */
                {
                    int (*fn)(void *) = *(int (**)(void *))(s + 0x20);
                    if (fn(s) != 0 && (s[0x24] & 1u))
                        if (spi_queue_send(s, 0, g_can_tick, NULL, 0, 0, 0) == 0) {
                            port_set_interrupt_mask();
                            for (;;) { }
                        }
                }
                break;
            }
            case 3: case 8:                             /* clear pending bit */
                frame[0x24] &= ~1u;
                break;
            case 4: case 9:                             /* (re)arm timeout fn */
                *(void **)(frame + 0x18) = d;           /* the 4th fn arg, not frame */
                vListInsert(frame, g_can_tick, g_can_tick);
                break;
            case 5:                                     /* free if not in-use */
                if ((frame[0x24] & 2u) == 0)
                    prvFreeBlock(frame);
                break;
            default:
                break;
            }
        }
    }
}
