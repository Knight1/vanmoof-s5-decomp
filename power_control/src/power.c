/*
 * power.c — VanMoof power_control power-mode state machine + telemetry.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin   (image base 0x0)
 *
 * Functions (OEM address order):
 *   power_send_mode_command        @ 0x00003200
 *   power_send_capabilities_frame  @ 0x00003234
 *   power_build_status_report      @ 0x00003730
 *   power_log_event                @ 0x00003980
 *   power_rail_enable_sequence     @ 0x00003b60
 *   power_set_mode                 @ 0x00003c94
 */

#include "power_control.h"

/*
 * power_send_mode_command — build a single power-mode command frame {opcode, 0}
 * and transmit it on the CAN object 0x1a4 (mode-request) channel. // 0x00003200
 *
 * OEM disassembly (0x00003200..0x0000322f):
 *
 * Reads the messaging handle from POWER_CAN_CTX, acquires a TX scratch buffer
 * for object 0x1a4 (can_request_obj_1a4). On success it writes the requested
 * mode opcode into byte 0, clears byte 1, and commits with can_send_obj_1a4.
 * Returns 0 on success, -1 on acquire failure or a commit error.
 */
int power_send_mode_command(uint32_t opcode)
{
    uint8_t *buf;
    uint32_t handle = *(volatile uint32_t *)POWER_CAN_CTX;
    int rc;

    rc = can_request_obj_1a4(handle, &buf);
    if (rc != 0) {
        return -1;
    }
    buf[0] = (uint8_t)opcode;
    buf[1] = 0;
    rc = can_send_obj_1a4(handle);
    return -(rc != 0);
}

/*
 * power_send_capabilities_frame — build a capability/limit frame on CAN object
 * 0x10a7 and transmit it. // 0x00003234
 *
 * OEM disassembly (0x00003234..0x00003297):
 *
 * Acquires a TX buffer for object 0x10a7. If both arguments are zero this is a
 * type-1 default advertisement (byte0=1, fixed maxima 0x00b8/0x0088 at +1/+3).
 * Otherwise it is a type-2 clamped response: it requires the two advertised
 * maxima in POWER_CAP_CTX (+0x16/+0x18) to be non-zero, writes byte0=2 and
 * stores the two requested values at +1/+3, clamping each down to its maximum.
 * Commits via can_send_obj_10a7.
 */
void power_send_capabilities_frame(uint32_t req_a, uint32_t req_b)
{
    uint8_t *buf;
    uint32_t handle = *(volatile uint32_t *)POWER_CAP_CTX;
    uint16_t max_a, max_b;

    can_request_obj_10a7(handle, &buf);
    if (req_a == 0 && req_b == 0) {
        buf[0] = 1;
        buf[1] = 0xb8;
        buf[2] = 0x88;
        buf[3] = 0;
        buf[4] = 0;
    } else {
        max_a = *(volatile uint16_t *)(POWER_CAP_CTX + 0x16u);
        if (max_a == 0) {
            return;
        }
        max_b = *(volatile uint16_t *)(POWER_CAP_CTX + 0x18u);
        if (max_b == 0) {
            return;
        }
        *(uint16_t *)(buf + 1) = (uint16_t)req_a;
        if (max_a < req_a) {
            *(uint16_t *)(buf + 1) = max_a;
        }
        *(uint16_t *)(buf + 3) = (uint16_t)req_b;
        buf[0] = 2;
        if (max_b < req_b) {
            *(uint16_t *)(buf + 3) = max_b;
        }
    }
    can_send_obj_10a7(handle);
}

/*
 * power_build_status_report — pack the live rail/flag state into a 2-byte status
 * bitfield message (opcode 0x2a3) and emit it, lazily arming a 1000ms watchdog
 * timer. // 0x00003730
 *
 * OEM disassembly (0x00003730..0x0000384c):
 *
 * Acquires a messaging frame of opcode 0x2a3 and waits up to 100 ticks for the
 * buffer; on failure it asserts (clears a flag at address 0x2 and traps). It
 * packs bits from the live state block (POWER_STATE): byte 2 = { state[0xd]!=0,
 * state[0x14]!=0, state[0x16]==0, state[0x20]==0, state[0xa]!=0 } in bits 0..4;
 * byte 1 = { state[0x24]!=0 (bit0), state[0x1a]!=0 (bit1), state[0x1b]!=0
 * (bit2) }. Byte 0 takes the caller sequence value. A second acquire publishes
 * the same opcode; if that frame has no timer yet (+0x1c == 0) it allocates 12
 * bytes and arms a 1000ms watchdog timer before recording the event and
 * committing.
 */
void power_build_status_report(uint32_t handle, uint32_t seq, uint32_t arg3)
{
    void **frame;
    uint8_t *p;
    volatile uint8_t *st = (volatile uint8_t *)POWER_STATE;
    void *frame2;
    void *timer;

    frame = (void **)comms_registry_lookup(handle, 0x2a3, (void *)(uintptr_t)arg3, 0);
    if (frame == 0 || comms_wait(((uint32_t *)frame)[1], 100) != 0) {
        *(volatile uint8_t *)0x00000002u &= 0xfe;
        __builtin_trap();   /* udf #0xff @0x00003848 (assert) */
    }

    p = (uint8_t *)frame[0];
    p[2] = (uint8_t)((p[2] & 0xe0)
         | ((st[0x0d] != 0) ? 0x01 : 0)
         | ((st[0x14] != 0) ? 0x02 : 0)
         | ((st[0x16] == 0) ? 0x04 : 0)
         | ((st[0x20] == 0) ? 0x08 : 0)
         | ((st[0x0a] != 0) ? 0x10 : 0));
    p[1] = (uint8_t)((p[1] & 0xf8)
         | ((st[0x24] != 0) ? 0x01 : 0)
         | ((st[0x1a] != 0) ? 0x02 : 0)
         | ((st[0x1b] != 0) ? 0x04 : 0));
    p[0] = (uint8_t)seq;

    *(volatile uint8_t *)POWER_STATUS_MIRROR = (uint8_t)seq;
    frame2 = comms_registry_lookup(handle, 0x2a3, 0, 0);
    if (frame2 == 0) {
        return;
    }
    if (*(int *)((char *)frame2 + 0x1c) == 0) {
        timer = pvPortMalloc(0xc);
        *(void **)((char *)frame2 + 0x1c) = timer;
        if (timer == 0) {
            return;
        }
        if (power_watchdog_timer_create(timer, 1000, handle, (void *)0x00003fc1u) != 0) {
            return;
        }
    }
    can_build_event_record((int)handle, (int)(uintptr_t)frame2);
    comms_release(*(uint32_t *)((char *)frame2 + 4));
}

/*
 * power_log_event — emit an event/status record: zero a 30-byte record, copy the
 * variadic payload words, stamp the source tag and event code, then publish.
 * // 0x00003980
 *
 * OEM disassembly (0x00003980..0x000039dd):
 *
 * No-op unless the event subsystem is live (POWER_EVENT_CTX holds a non-null
 * context). Otherwise zeroes a 30-byte record laid out as { src_tag@0 (u32),
 * code@4 (u16), body@6 (argc u32 words copied from the variadic tail) } and
 * publishes it to the event queue at *(*ctx)+0x590 using the descriptor 0x3d31.
 */
void power_log_event(uint32_t src_tag, uint16_t code, int argc, ...)
{
    uint32_t *ctx = (uint32_t *)POWER_EVENT_CTX;

    if (*ctx != 0) {
        uint32_t rec[8];                                  /* 0x1e-byte record + slack */
        uint8_t  *r = (uint8_t *)rec;
        uint32_t *body = (uint32_t *)(r + 0x6);
        uint32_t *args = (uint32_t *)((char *)&argc + 4); /* variadic tail (OEM stack walk) */
        int i;

        mem_set(rec, 0, 0x1e);
        for (i = 0; i != argc; i++) {
            body[i] = args[i];
        }
        *(uint32_t *)r = src_tag;
        *(uint16_t *)(r + 0x4) = code;
        event_queue_publish(*(uint32_t *)(*(uint32_t *)*ctx + 0x590),
                            0x00003d31u, 0, rec, 0x1e);
    }
}

/*
 * power_rail_enable_sequence — power a rail/peer up with up to 3 enable attempts;
 * log success (0xac) or exhaustion failure (0xb2). // 0x00003b60
 *
 * OEM disassembly (0x00003b60..0x00003be9):
 *
 * Reads the rail control structure from POWER_CAN_CTX+0x20; if absent it panics.
 * Under a critical section it clears bit0 of the control word, then loops up to
 * three times: asserts the enable strobe (state[0xe]=1), delays the settle time
 * in ms, deasserts the strobe, and waits up to 1000 ticks for a task notify. If
 * bit0 of the notify is set the rail is up: it delays a further 500ms, logs
 * event 0xac (attempt count) and returns 0. After 3 failures it logs 0xb2 and
 * returns -1.
 */
int power_rail_enable_sequence(int settle_ms)
{
    uint32_t *ctrl = *(uint32_t **)(POWER_CAN_CTX + 0x20u);
    volatile uint8_t *st = (volatile uint8_t *)POWER_STATE;
    int attempt = 0;

    if (ctrl == 0) {
        power_panic();
        for (;;) { }
    }
    vPortEnterCritical();
    *ctrl &= 0xfffffffeu;
    vPortExitCritical();

    do {
        st[0x0e] = 1;
        rtos_task_delay((uint32_t)(settle_ms * 1000) / 1000u);
        st[0x0e] = 0;
        attempt++;
        if ((rtos_task_notify_wait(*(uint32_t *)(POWER_CAN_CTX + 0x20u), 1, 1, 1000) & 1) != 0) {
            rtos_task_delay(500);
            power_log_event(0xa2e8e4a6u, 0xac, 1, attempt);
            return 0;
        }
    } while (attempt != 3);

    power_log_event(0xa2e8e4a6u, 0xb2, 0);
    return -1;
}

/*
 * power_set_mode — core power-mode negotiation state machine: send the mode
 * command, wait for the achieved-mode ack with a per-mode timeout, compare
 * achieved vs requested, and retry. Returns 0 on success, -1 on give-up.
 * // 0x00003c94
 *
 * OEM disassembly (0x00003c94..0x00003d24):
 *
 * For any mode other than 3 it first brings up the rail
 * (power_rail_enable_sequence(0x44c)). It then runs an outer retry loop (rounds
 * 1..3) and an inner attempt loop (1..3): each outer iteration sends the mode
 * command; each inner attempt logs trace event 0xf6 and waits up to 600 ticks
 * for a notify on the handle at POWER_CAN_CTX+0x20 (clear-on-exit only when
 * mode!=3). If bit0 of the notify is set it reads the achieved mode (low nibble
 * of state[0xa]), logs 0xfe, and returns 0 if it equals the requested mode; if
 * the signed status byte state[4] is negative it returns -1. After 3 rounds it
 * logs 0x109 and returns -1.
 */
int power_set_mode(uint32_t mode)
{
    int ctx = (int)POWER_CAN_CTX;
    uint32_t round = 1;
    int attempt;
    uint32_t notify;
    uint32_t achieved = round;

    if (mode != 3) {
        power_rail_enable_sequence(0x44c);
    }
    for (;;) {
        power_send_mode_command(mode);
        attempt = 0;
        do {
            attempt++;
            achieved = round;
            power_log_event(0xa2e8e4a6u, 0xf6, 3, mode, round, attempt);
            notify = rtos_task_notify_wait(*(uint32_t *)(ctx + 0x20), 1,
                                           (uint32_t)(mode != 3), 600);
            if ((notify & 1) != 0) {
                uint8_t cur = *(volatile uint8_t *)(ctx + 0xa);
                achieved = cur & 0xf;
                power_log_event(0xa2e8e4a6u, 0xfe, 2, (uint32_t)cur, achieved, attempt);
                if (mode == (uint32_t)(cur & 0xf)) {
                    return 0;
                }
                if ((signed char)*(volatile uint8_t *)(ctx + 4) < 0) {
                    return -1;
                }
            }
        } while (attempt != 3);
        round++;
        if (round == 4) {
            power_log_event(0xa2e8e4a6u, 0x109, 0, 0, achieved, attempt);
            return -1;
        }
    }
}
