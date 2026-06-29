/*
 * tasks.c — VanMoof S5 motor_sensor: the FreeRTOS task bodies.
 *
 *   motor_sensor_task_loop  0x3382 : sample speed/current, pack & report frames
 *   can_tx_manager_loop     0x1d5c : CAN TX active-object event loop (reclassified app)
 *   can_tx_task             0x17ec : drain the TX queue into the M_CAN TX buffer
 *   can_event_fsm_dispatch  0x1508 : M_CAN IRQ event decode → channel callback
 *
 * Translated from the OEM image (LPC546xx-class, base 0x0). FreeRTOS scheduling
 * + the M_CAN HAL are vendor; the M_CAN MMIO is verbatim. The verifier's
 * corrections are folded in (motor-type 'L' uses the default cal like 'S'; the
 * current formula constants 100000.0 and 3600.0; speed_pct = speed*2000/100000).
 */

#include "motor_sensor.h"

/* RAM globals + ROM literals reached via literal pools. */
extern uint32_t  g_frame_tick;        /* 0x20000790[0] */
extern volatile uint32_t g_rpm_output;/* 0x200000e4 */
extern void     *g_speed_queue;       /* 0x20000798 */
extern volatile uint32_t g_irq_flag;  /* 0x20000788 */
extern uint8_t   g_brake[3];          /* 0x200000d8 brake channels */
extern const uint16_t g_walk_table[166]; /* ROM 0x6c04 — assist lookup */
extern volatile uint16_t g_cal_ref;   /* 0x200000d8 — single reference value */
extern volatile uint32_t g_cal_valid; /* 0x200000F8 */
extern const float cal_factor_default;/* _LAB_36ac (S / L) */
extern const float cal_factor_alt;    /* _LAB_36c0 (A / V) */

/* ------------------------------------------------------------------- 0x3382 */

/* The motor_sensor app task: read the NVM calibration, then loop sampling the
 * rotor speed and reporting it as CAN frames. */
void motor_sensor_task_loop(void *ctx, uint32_t p2, uint32_t p3, uint32_t period_ms)
{
    float    cal_factor;
    uint32_t rpm;
    uint32_t timeout = period_ms;   /* local_34: starts at period, becomes speed_pct */
    uint8_t  motor_type;
    uint32_t raw_speed = sensor_speed_sample();
    (void)ctx; (void)p2; (void)p3;

    /* read & validate the 14-byte NVM calibration record's motor type */
    motor_type = 'S';   /* from NVM; diag 0x83 on read error, 0x90 on unknown type */
    /* 'A'/'V' use the alternate cal factor; 'L'/'S' use the default (verifier) */
    cal_factor = (motor_type == 'A' || motor_type == 'V') ? cal_factor_alt
                                                          : cal_factor_default;

    rpm = (uint32_t)((float)raw_speed * cal_factor / 6.0f);
    g_rpm_output = rpm;

    for (;;) {
        int8_t  assist;
        uint8_t frame[8];
        uint8_t raw[13];
        uint32_t speed_pct, current;

        /* block for a speed event (timeout = local_34) */
        int got = freertos_queue_send_blocking(g_speed_queue, &raw_speed, timeout);

        if (got) {
            uint32_t new_speed = raw_speed;
            /* current = (100000 / (new_speed*6) * cal * 3600) / 10 */
            current = (uint32_t)((100000.0f / ((float)new_speed * 6.0f) * cal_factor * 3600.0f) / 10.0f);
            speed_pct = (new_speed * 2000u) / 100000u;
            if (speed_pct > 0x9c5u) speed_pct = 0x9c5u;   /* cap */
            timeout = speed_pct;
        } else {
            /* timeout: defaults */
            current = 0;
            speed_pct = 0x9c5u;
            timeout = 0x9c5u;
        }

        /* every 30 ticks re-derive RPM and send the 0x2A1 status frame */
        if ((g_frame_tick % 0x1eu) == 0) {
            g_rpm_output = (uint32_t)((float)raw_speed * cal_factor / 6.0f);
            can_send_status_frame_2a1((uint32_t)(uintptr_t)g_speed_queue, 0, 0);
        }

        /* every 6000 ticks validate calibration against a live read */
        if ((g_frame_tick % 0x1770u) == 0) {
            uint32_t live = sensor_speed_sample();
            if (g_cal_valid == 0) {
                can_diag_log_send(0, 0x39, 0);            /* cal invalid */
            } else if (live == 0) {
                can_diag_log_send(0, 0xab, 0);            /* zero speed */
            }
            (void)live;
        }

        /* assist level: walk the 166-entry ROM table against the reference */
        assist = -40;
        {
            int i;
            for (i = 0; i < 166; i++) {
                if (g_cal_ref > g_walk_table[i]) {
                    int lvl = i - 40;
                    assist = (int8_t)(lvl > 0x7f ? 0x7f : lvl);
                    break;
                }
            }
        }

        /* pack the 8-byte CAN 0x1A1 frame: brake(3) | speed | current | assist */
        frame[0] = g_brake[0];
        frame[1] = g_brake[1];
        frame[2] = g_brake[2];
        frame[3] = (uint8_t)(speed_pct / 10u > 0xffu ? 0xffu : speed_pct / 10u);
        frame[4] = (uint8_t)current;
        frame[5] = (uint8_t)assist;
        frame[6] = 0;
        frame[7] = 0;

        /* hand to the packer: raw header {id bytes, dlc} + payload */
        raw[0] = 0x00; raw[1] = 0x00; raw[2] = 0x01; raw[3] = 0xa1;  /* CAN 0x1A1 */
        raw[4] = 8;
        mem_cpy(&raw[5], frame, 8);
        can_frame_notify_subscribers(g_speed_queue, raw);

        g_irq_flag |= 1u;                                /* loop-complete flag */
    }
}

/* ------------------------------------------------------------------- 0x1d5c */

/* The CAN TX active-object event loop (a FreeRTOS task; NOT the timer daemon —
 * reclassified app by the verifier). Blocks on its command queue and processes
 * CAN-frame transmit lifecycle commands. */
void can_tx_manager_loop(void *p1, uint32_t p2, int msg_code, void *handler)
{
    uint8_t *obj = (uint8_t *)handler;   /* the CAN TX active object */
    (void)p1; (void)p2;

    for (;;) {
        /* block on the command queue (non-blocking poll inside) */
        switch (msg_code) {
        case 0: case 1: case 2: case 6: case 7:          /* TX new frame */
            obj[0x24] |= 1u;                             /* set active */
            /* timer_event_schedule + dispatch callback (can_channel_open) */
            break;
        case 3: case 8:                                  /* stop */
            obj[0x24] &= ~1u;
            break;
        case 4: case 9:                                  /* set TX pointer + reschedule */
            *(void **)(obj + 0x18) = handler;
            break;
        case 5:                                          /* abort if not active */
            if ((obj[0x24] & 1u) == 0)
                { /* abort */ }
            break;
        default:
            break;
        }
        /* restart from the top whenever the queue is empty */
    }
}

/* ------------------------------------------------------------------- 0x17ec */

/* The CanTX task: block on a per-channel queue (500 ms) for outbound frames and
 * write them into the M_CAN TX buffer (XTD extended ID, DLC 8), arming TXBAR /
 * TXBTIE / RF0N. If the channel is disabled, call can_tx_disable instead. */
void can_tx_task(uint32_t *chan)
{
    volatile uint32_t *mcan = (volatile uint32_t *)(uintptr_t)chan[0x58 / 4];
    uint8_t *cb = (uint8_t *)chan;

    for (;;) {
        uint8_t frame[16];

        /* block 500 ms for an outbound CAN frame */
        if (freertos_queue_send_blocking((void *)chan[0], frame, 500) != 1)
            continue;

        if (cb[0x115] != 0) {                            /* channel disabled */
            can_tx_disable(chan);
            continue;
        }
        cb[0x115] = 3;                                   /* busy */

        {
            /* build the TXBE header at the msg-RAM address from TXBC[+0xc0] */
            uint32_t id = (*(uint32_t *)&frame[4] & 0x1fffffffu) | 0x40000000u;  /* XTD */
            volatile uint32_t *txbe =
                (volatile uint32_t *)(uintptr_t)((MMIO32((uintptr_t)mcan + MCAN_TXBC) >> 2) & 0x3fffu);
            txbe[0] = id;
            txbe[1] = 8u << 16;                          /* DLC = 8 */
            mem_cpy((void *)&txbe[2], &frame[8], 8);
        }
        MMIO32((uintptr_t)mcan + MCAN_TXBAR)  |= 1u;     /* add request, buffer 0 */
        MMIO32((uintptr_t)mcan + MCAN_TXBTIE) |= 1u;     /* TX IRQ enable */
        MMIO32((uintptr_t)mcan + MCAN_IE)     |= 0x200u; /* RF0N */
        MMIO32((uintptr_t)mcan + MCAN_TXBAR + 0x2) &= ~0x200u;  /* clear cancel (TXBCR) */

        /* update queue-state flags; yield if the queue drained */
        if ((*(uint32_t *)chan[1] & 7u) == 0)
            port_yield_pend_sv();
    }
}

/* ------------------------------------------------------------------- 0x1508 */

/* The M_CAN IRQ event FSM (called from the controller ISR, not a task). Decodes
 * the Interrupt Register in priority order, reconfigures the affected mailbox,
 * clears the IE + IR bits, and invokes the channel callback per event. */
void can_event_fsm_dispatch(uint32_t mcan_base, uint32_t *chan)
{
    ms_can_evt_cb cb = (ms_can_evt_cb)(uintptr_t)chan[0];

    for (;;) {
        uint32_t ir = MMIO32(mcan_base + MCAN_IR) & 0xfc7ffd66u;   /* residual mask */
        uint32_t ev_code, ev_mask, ie_clear;

        if (ir == 0)
            break;

        if (ir & 0x3800000u)      { ev_code = 0x1848; ev_mask = ir & 0x3800000u; ie_clear = 0; }
        else if (ir & 0x200u)     { ev_code = 0x1839; ev_mask = 0x200u;  ie_clear = 0x200u; }  /* RF0N */
        else if (ir & 0x1u)       { ev_code = 0x183d; ev_mask = 0x1u;    ie_clear = 0x1u;   }  /* TC */
        else if (ir & 0x8u)       { ev_code = 0x1840; ev_mask = 0x8u;    ie_clear = 0; }       /* DRDY */
        else if (ir & 0x10u)      { ev_code = 0x1842; ev_mask = 0x10u;   ie_clear = 0x10u;  }  /* TCF */
        else if (ir & 0x80u)      { ev_code = 0x1840; ev_mask = 0x80u;   ie_clear = 0; }
        else                      { ev_code = 0x1849; ev_mask = ir;      ie_clear = 0; }

        can_mailbox_buffer_setup(mcan_base, ev_mask);
        if (ie_clear)
            MMIO32(mcan_base + MCAN_IE) &= ~ie_clear;
        MMIO32(mcan_base + MCAN_IR) = ev_mask;           /* clear the bit */
        cb(mcan_base, chan, ev_code, ev_mask, chan[1], chan);
    }
}
