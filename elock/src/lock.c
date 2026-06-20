/*
 * lock.c — the core electronic frame-lock FreeRTOS task (elock).
 *
 * VanMoof S5/A5 electronic frame-lock controller. elock_lock_task runs the lock
 * state machine (UNKNOWN/LOCKED/UNLOCKED/STUCK, broadcast on CAN OD 0x4c1),
 * drives the lock actuator motor, and runs the anti-theft accelerometer tilt
 * alarm (OD 0x7c1, two-pole IIR, threshold 7.0). Translated from the OEM image
 * (NXP LPC546xx Cortex-M4F, image base 0x0).
 *
 * The M_CAN bring-up register pokes and the OD/CAN comms-registry calls
 * (od_registry_*, od_frame_*, msgbuf_*, the registry teardown) are NXP SDK/HAL +
 * the generic CAN Object-Dictionary middleware (vendor, declared extern). Only
 * the state machine, the tilt filter and the event/CAN signalling are VanMoof.
 * MMIO and RAM-global accesses are done verbatim against the device addresses.
 *
 * Lock context layout (param `t`, a uint32_t* at +0x0):
 *   +0x00 OD/comms registry context pointer
 *   +0x04 comms sub-context (M_CAN registry node, passed to the bus helpers)
 *   +0x14 task run-state byte (1=run,2=init,3=halt-req,10=restart)
 *   +0x18 CAN-bus watchdog sub-handle
 *   +0x1c loop/idle counter
 *   +0x20 accel X word, +0x24 angle (int16)
 *   +0x28 lock state byte (current), +0x29 target state byte
 *   +0x2c lock/unlock debounce counter (reaches 500)
 *   +0x30 retry timer handle, +0x34 retry-active flag, +0x38 retry countdown,
 *   +0x3c lock-engage angle threshold, +0x40 unlock angle threshold,
 *   +0x44 slow IIR tilt filter (float), +0x48 fast IIR tilt filter (float),
 *   +0x4c anti-theft alarm armed flag, +0x4d retry-in-progress byte,
 *   +0x50 unlock retry counter, +0x54 over-threshold sample counter,
 *   +0x58 100ms broadcast divider counter
 */

#include "elock.h"

void elock_lock_task(uint32_t *t, uint32_t arg1)
{
    void   *comms = (void *)(t + 1);                 /* ctx+0x04 */
    int32_t init_failed;
    int32_t loops;

    (void)arg1;

    rtos_task_delay(10);

    /* ---- M_CAN / driver bring-up (vendor HAL + OD middleware) ---- */
    {
        int *can = (int *)0x20000124;                /* lock_task_can_block */
        int  ctrl_idx, ok = 1;
        t[6] = (uint32_t)can;                        /* park task ptr (ctx+0x18) */
        *(volatile uint8_t *)0xe000e40fu = 0x60;     /* NVIC IPR byte */
        mem_set(can, 0, 0x4c);
        can[0x11] = (int)msgbuf_create();            /* RX buffer */
        if (can[0x11] != 0) {
            can[0x12] = (int)rtos_msgbuf_alloc(1, 0); /* TX buffer */
            if (can[0x12] != 0) {
                volatile uint32_t *mcan = (volatile uint32_t *)0x40087000u;
                can[0] = 0x40087000;
                ctrl_idx = m_can_index(mcan);
                m_can_set_baud(*(uint16_t *)(0x0000789c + ctrl_idx * 2));
                m_can_set_clock(*(uint32_t *)(0x00007878 + ctrl_idx * 4));
                if (((int32_t)(mcan[0x3fe] << 0x19) < 0) &&
                    ((int32_t)(mcan[0x3fe] << 0x1c) >= 0 || (mcan[0x3fe] & 7) == 3))
                    mcan[0x3fe] = 3;                 /* +0xff8 CCCR */
                mcan[0x200] = (mcan[0x200] & 0x1e) | 1; /* +0x800 */
                mcan[0x205] = 2;                     /* +0x814 */
                mcan[0x209] = 1;                     /* +0x824 */
                mcan[0x204] = 0xffff;                /* +0x810 */
                mem_set((void *)0x20000128, 0, 0x3c);/* RX fifo scratch */
                can[0x0e] = 0x6815;                  /* ISR entry */
                can[0x0f] = (int)can;
                *(uint32_t *)(0x2000024c + ctrl_idx * 4) = 0x20000128;
                *(uint32_t *)(0x20000270 + ctrl_idx * 4) = 0x000012ed;
                mcan[0x203] = 0x03000051;            /* +0x80c IE */
                nvic_enable(0xf);
                ok = 0;
            } else {
                rtos_msgbuf_delete(can[0x11]);
            }
        }
        init_failed = ok ? 1 : 0;
    }

    if (init_failed) {
        elock_log_event(0xe3707501, 0xf8, 0);
    } else {
        int i = 0;
        *(uint8_t *)((char *)t + 0x14) = 2;
        do {
            init_failed = elock_can_bus_reset(comms);
            if (init_failed) break;
            elock_log_event(0xe3707501, 0x106, 1, i);
        } while (++i != 3);
    }

    /* ---- failure: tear the comms node down and delete the task ---- */
    if (init_failed) {
        registry_teardown_enter(2, 0, 0);
        rtos_lock_a();
        {
            int *node = (int *)0x20000180;
            int  n = *node;
            mb_reclaim(n + 4);
            if (*(int *)(n + 0x28) != 0)
                mb_reclaim(n + 0x18);
            mb_assign(0x2000097c, n + 4);
            if (*(char *)(n + 0x68) == 1)
                *(uint8_t *)(n + 0x68) = 0;
            rtos_unlock_a();
            if (*(int *)0x20000970 != 0) {
                rtos_lock_a();
                registry_purge();
                rtos_unlock_a();
            }
            if (n == *node) {
                if (*(int *)0x20000970 == 0) {
                    if (*(int *)0x2000097c == *(int *)0x200002d4)
                        *node = 0;
                    else
                        registry_finalize();
                } else {
                    rtos_assert();              /* never returns */
                    rtos_yield_isr();
                }
            }
        }
    }

    /* ---- steady-state loop ---- */
    for (;;) {
        uint8_t  *p8 = (uint8_t *)t;
        uint16_t  frame[4];                          /* 4x int16 sample (8 bytes) */
        int       got;

        do {
            got = rtos_msgbuf_receive(*(void **)0x200002cc, 1);
            if (got) {
                *(uint8_t *)((char *)t + 0x14) = 2;
                od_frame_flush(comms);
            }
        } while (*(char *)((char *)t + 0x14) == 1);

        frame[0] = 0; frame[1] = 0; frame[2] = 0; frame[3] = 0;
        rtos_msgbuf_receive(*(void **)0x200002c8, 0xffffffffu);
        got = od_frame_get(comms, 6);
        if (got == 0) {
            uint8_t  fl = p8[9];
            uint16_t fx = (uint16_t)(p8[6] << 4);
            uint16_t fy = (uint16_t)(p8[7] << 4);
            uint16_t fa = (uint16_t)(p8[8] << 4);
            short    ang;
            if (*(char *)((char *)t + 0x14) == 3) {
                ang = 0;
            } else {
                fx |= (uint16_t)(p8[10] >> 4);
                fy |= (uint16_t)(p8[10] & 0xf);
                fa |= (uint16_t)(p8[0xb] & 0xf);
                ang = (short)((uint16_t)p8[3] + ((fl & 0xf0) << 4));
            }
            if ((uint32_t)p8[5] == (uint32_t)((fl & 0xf) >> 2)) {
                elock_can_bus_reset(comms);      /* matching state request -> bus reset */
                got = 1;
                *(uint8_t *)((char *)t + 0x14) = 10;
            } else {
                *(uint8_t *)((char *)t + 0x14) = (uint8_t)(((uint32_t)fl << 0x1c) >> 0x1e);
                if ((int)((uint32_t)fx << 0x14) < 0)
                    fx = (uint16_t)~((uint16_t)~(uint16_t)(((uint32_t)fx << 0x14) >> 0x10) >> 4);
                if ((int)((uint32_t)fy << 0x14) < 0)
                    fy = (uint16_t)~((uint16_t)~(uint16_t)(((uint32_t)fy << 0x14) >> 0x10) >> 4);
                if ((int)((uint32_t)fa << 0x14) < 0)
                    fa = (uint16_t)~((uint16_t)~(uint16_t)(((uint32_t)fa << 0x14) >> 0x10) >> 4);
                frame[0] = fx;
                frame[1] = fy;
                frame[2] = fa;
                frame[3] = (uint16_t)ang;
            }
        }
        rtos_msgbuf_send(*(void **)0x200002c8, 0);

        if (got == 0) {
            t[7] = 0;
            mem_copy(t + 8, frame, 8);            /* sample @ +0x20 */
            {
                uint32_t c = t[0x16];
                t[0x16] = c + 1;
                if ((c + 1) % 100 == 0) {
                    /* broadcast live motor/accel sample on OD 0x2c1 */
                    int *e = (int *)od_registry_lookup(*t, 0x2c1);
                    if (e && od_registry_wait((void *)e[1], 100) == 0) {
                        uint32_t *pl = (uint32_t *)*e;
                        pl[0] = t[8];
                        pl[1] = t[9];
                        int e2 = (int)od_registry_lookup(*t, 0x2c1);
                        if (e2) od_registry_release(*(void **)(uintptr_t)(e2 + 4));
                    }
                }
            }

            /* lock/unlock angle debounce -> target state @ +0x29 */
            {
                short ang = *(short *)((char *)t + 0x24);
                uint8_t prev = *(uint8_t *)((char *)t + 0x29);
                uint8_t tgt;
                if ((int)ang < (int)t[0xf]) {
                    t[0xb] = (prev == 1) ? (int)t[0xb] + 1 : 0;
                    tgt = 1;
                } else {
                    t[0xb] = (prev == 3) ? (int)t[0xb] + 1 : 0;
                    tgt = 3;
                }
                *(uint8_t *)((char *)t + 0x29) = tgt;
            }

            /* retry / motor-settle handling */
            {
                uint8_t  cur = *(uint8_t *)((char *)t + 0x28);
                char     rip = *(char *)((char *)t + 0x4d);
                short    ang = *(short *)((char *)t + 0x24);
                if (cur != 1 || rip == 1) {
                    if (t[0xd] != 0) {
                        int rem = (int)t[0xe] - 1;
                        t[0xe] = rem;
                        if (rem < 1) {
                            t[0xd] = 0;
                        } else if ((uint32_t)t[0xd] > 100) {
                            if ((int)ang < (int)t[0x10])
                                t[0x15] = t[0x15] + 1;
                            if (rip == 0 && t[0x15] != 0) {
                                t[0x15] = 0;
                                *(uint8_t *)((char *)t + 0x4d) = 1;
                                elock_unlock_retry_arm(t);
                            }
                            goto retry_done;
                        }
                    }
                    if (rip == 1) {
                        timer_stop((void *)(uintptr_t)t[0xc]);
                        *(volatile uint32_t *)(0x40008000u + 4u) &= ~1u;
                        lock_motor_pwm_set_duty(0);
                        *(uint8_t *)((char *)t + 0x4d) = 0;
                    }
                }
            }
        retry_done:

            /* anti-theft tilt filter (two-pole IIR, threshold 7.0) */
            if (*(char *)((char *)t + 0x4c) != 0) {
                float ang = (float)*(short *)((char *)t + 0x24);
                float f1  = ang * 0.02f + ((float *)t)[0x11] * 0.98f;   /* slow @+0x44 */
                float f2  = ang * 0.2f  + ((float *)t)[0x12] * 0.8f;    /* fast @+0x48 */
                int   now = *(int *)0x200009a4;
                int   ts  = *(int *)0x20000178;
                ((float *)t)[0x11] = f1;
                ((float *)t)[0x12] = f2;
                if ((uint32_t)(now - ts) > 4999u) {
                    if ((f1 - f2) >= 7.0f) {
                        timer_stop((void *)0x20000098);
                        {
                            uint32_t od = *t;
                            int e = (int)od_registry_lookup(od, 0x7c1);
                            if (e && od_registry_wait(*(void **)(uintptr_t)(e + 4), 100) == 0) {
                                int e2 = (int)od_registry_lookup(od, 0x7c1);
                                if (e2) {
                                    elock_can_send_signal((void *)od, (elock_od_record_t *)(uintptr_t)e2);
                                    od_registry_release(*(void **)(uintptr_t)(e2 + 4));
                                }
                            }
                        }
                        *(int *)0x20000178 = *(int *)0x200009a4;
                    }
                }
            }

            /* lock-state-machine completion via the holdoff timer @0x20000098 */
            {
                int tmr = (int)0x20000098;
                if (*(int *)(uintptr_t)(tmr + 4) == 1) {
                    if (*(char *)((char *)t + 0x28) == *(char *)((char *)t + 0x29)) {
                        *(uint8_t *)((char *)t + 0x14) = 1;
                        od_frame_flush(comms);
                        timer_complete_cb(*(void **)(uintptr_t)(tmr + 0x10),
                                          *(void **)(uintptr_t)(tmr + 8),
                                          *(void **)(uintptr_t)(tmr + 0xc));
                    } else {
                        timer_stop((void *)0x20000098);
                    }
                }
            }

            /* commit lock state once debounce reaches 500 -> broadcast OD 0x4c1 */
            if ((uint32_t)t[0xb] > 499u) {
                uint32_t od = *t;
                *(uint8_t *)((char *)t + 0x28) = *(uint8_t *)((char *)t + 0x29);
                t[0xb] = 0;
                {
                    uint32_t *e = (uint32_t *)od_registry_lookup(od, 0x4c1);
                    if (e && od_registry_wait((void *)e[1], 100) == 0) {
                        *(uint8_t *)(uintptr_t)e[0] = *(uint8_t *)((char *)t + 0x28);
                        int e2 = (int)od_registry_lookup(od, 0x4c1);
                        if (e2) {
                            elock_can_send_signal((void *)od, (elock_od_record_t *)(uintptr_t)e2);
                            od_registry_release(*(void **)(uintptr_t)(e2 + 4));
                        }
                    }
                }
                if (*(char *)((char *)t + 0x28) == 1) {
                    int *cnt = (int *)0x2000017c;
                    if (*cnt < 8 && (*cnt = *cnt + 1) == 8) {
                        *(uint8_t *)((char *)t + 0x4c) = 1;
                        ((float *)t)[0x11] = (float)*(short *)((char *)t + 0x24);
                        ((float *)t)[0x12] = (float)*(short *)((char *)t + 0x24);
                    }
                } else {
                    *(uint8_t *)((char *)t + 0x4c) = 0;
                    *(int *)0x2000017c = 0;
                }
                __asm volatile ("cpsid i");
                *(volatile uint32_t *)0x2000089c |= 1u;
                __asm volatile ("cpsie i");
            }
        }

        /* CAN-bus watchdog ping every >10 loops */
        loops = t[7];
        t[7] = loops + 1;
        if ((uint32_t)(loops + 1) > 10u) {
            int *wd = (int *)(uintptr_t)t[6];
            if (wd != 0) {
                if (rtos_msgbuf_receive((void *)(uintptr_t)wd[0x11], 0xffffffffu) == 1) {
                    if ((char)wd[1] != 0) {
                        int  m = wd[0];
                        int  rc;
                        *(uint32_t *)(uintptr_t)(m + 0x80c) = 0x03000051;
                        rc = m_can_apply(m);
                        if (rc == 0) {
                            if ((*(uint32_t *)(uintptr_t)(m + 0x804) & 0xe) != 0) {
                                *(uint32_t *)(uintptr_t)(m + 0x820) = 4;
                                if (m_can_apply(m) != 0) {
                                    *(uint8_t *)((char *)wd + 4) = 0;
                                    goto wd_give;
                                }
                            }
                            *(uint8_t *)((char *)wd + 4) = 0;
                            *(uint8_t *)((char *)wd + 0x1c) = 0;
                        } else {
                            *(uint8_t *)((char *)wd + 4) = 0;
                        }
                    }
                wd_give:
                    rtos_msgbuf_send((void *)(uintptr_t)wd[0x11], 0);
                }
                od_frame_flush(comms);
            }
        }

        /* liveness / heartbeat every >20 loops -> OD 0xba2 */
        if ((uint32_t)t[7] > 0x14u) {
            registry_teardown_enter(4, 0, 1);
            elock_log_event(0xcb0c699e, 0xf2, 0);
            {
                uint32_t od = *t;
                int e = (int)od_registry_lookup(od, 0xba2);
                if (e && od_registry_wait(*(void **)(uintptr_t)(e + 4), 100) == 0) {
                    int e2 = (int)od_registry_lookup(od, 0xba2);
                    if (e2) {
                        elock_can_send_signal((void *)od, (elock_od_record_t *)(uintptr_t)e2);
                        od_registry_release(*(void **)(uintptr_t)(e2 + 4));
                    }
                }
            }
            t[7] = 0;
        }
    }
}
