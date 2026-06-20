/*
 * motor.c — lock-actuator motor drive on the LPC SCTimer (elock).
 *
 * VanMoof S5/A5 electronic frame-lock controller. Functions are translated from
 * the OEM image (NXP LPC546xx Cortex-M4F, image base 0x0). The SCTimer/PWM
 * register pokes are NXP-SDK shaped but the duty arithmetic and the retry/settle
 * glue are VanMoof; MMIO is done verbatim against the device addresses.
 */

#include "elock.h"

/* lock_motor_pwm_set_duty — set the lock-actuator motor PWM duty (LPC SCTimer).
 *
 * OEM disassembly (0x000003cc..0x0000043a):
 *
 * The active SCT event index is held in a RAM global pointer at 0x200000b8
 * (sct_active_event_ctx). The routine first reads two SCTimer state/config
 * words: SCT_EV[*ctx].STATE (base 0x40085000 + *ctx*8 + 0x30c, used to derive
 * the output channel) and SCT_EV[*ctx].CTRL (0x40085000 + *ctx*8 + 0x304),
 * whose low nibble selects the match register; from that it loads the current
 * PWM period from the SCT match table at 0x40085000 + (0x40 + match)*4.
 *
 * For pct==100 (full duty) it does not divide: it nudges the reload value by
 * +2 or -1 depending on the SCT direction bit (CTRL word at base+4, bit 4 via
 * the <<0x1b sign test). Otherwise duty = (uint16_t)((pct * period) / 100),
 * computed as a 64-bit unsigned multiply/divide (__aeabi_uldivmod).
 *
 * The new reload is written to MATCHREL (base + chan*4 + 0x100) and to its
 * counterpart reload register (base + chan*4 + 0x200). Both stores are bracketed
 * by setting the SCT HALT/no-reload guard bit (base+4 |= 4) and clearing it
 * afterwards (base+4 &= ~4) so the match registers update atomically.
 */
void lock_motor_pwm_set_duty(unsigned int pct)
{
    /* sct_base = 0x40085000; sct_active_event_ctx @ 0x200000b8 */
    uint32_t state_idx  = *(volatile uint32_t *)(*(uint32_t *)0x200000b8u * 8u + 0x4008530cu);
    uint32_t ctrl       = *(volatile uint32_t *)(*(uint32_t *)0x200000b8u * 8u + 0x40085304u);
    uint32_t period     = *(volatile uint32_t *)(0x40085000u + ((ctrl & 0xfu) + 0x40u) * 4u);
    uint16_t reload;

    if (pct == 100u) {
        short cur = (short)period;
        if (((int)(*(volatile uint32_t *)(0x40085000u + 4u)) << 0x1b) < 0)
            reload = (uint16_t)(cur - 1);
        else
            reload = (uint16_t)(cur + 2);
    } else {
        uint64_t prod = (uint64_t)pct * (uint64_t)period;
        reload = (uint16_t)(prod / 100u);
    }

    /* set the match-register update guard, write MATCHREL + reload, then clear */
    *(volatile uint32_t *)(0x40085000u + 4u) |= 4u;
    {
        uint32_t chan = (state_idx & 0xfu) * 4u;
        *(volatile uint32_t *)(chan + 0x40085100u) = reload;
        *(volatile uint32_t *)(chan + 0x40085200u) = reload;
    }
    *(volatile uint32_t *)(0x40085000u + 4u) &= ~4u;
}

/* elock_unlock_retry_arm — decrement the unlock retry counter, give the motor
 * a brief settle drive, and re-arm the lock-retry timeout.
 *
 * OEM disassembly (0x00003450..0x0000347c):
 *
 * The lock-task context is passed in r0. The routine decrements the retry
 * counter at ctx+0x50, drives the lock actuator to 100% duty for a short
 * settling pulse (lock_motor_pwm_set_duty(100)), then sets bit0 of the motor
 * enable register at 0x40008004 (peripheral block 0x40008000) to power the
 * actuator. Finally it (re)arms the one-shot retry timeout via the FreeRTOS
 * software-timer wrapper (elock_timeout_arm) with a 0x96 (150) tick period,
 * one-shot flag 1, passing the lock context as the callback argument and the
 * source tag 0x4029; the timer handle lives at ctx+0x30.
 */
void elock_unlock_retry_arm(void *ctx)
{
    *(int *)((char *)ctx + 0x50) -= 1;
    lock_motor_pwm_set_duty(100);
    *(volatile uint32_t *)(0x40008000u + 4u) |= 1u;            /* motor enable */
    elock_timeout_arm(*(void **)((char *)ctx + 0x30), 0x96, 1, ctx, 0x4029);
}
