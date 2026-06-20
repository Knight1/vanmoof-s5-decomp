/*
 * led.c — front LED driver primitives (PWM duty, per-channel write, brightness).
 *
 * VanMoof S5/A5 front LED light controller. Translated from the OEM image
 * (NXP LPC546xx Cortex-M4F, image base 0x0). The SCTimer/PWM register pokes and
 * the FreeRTOS mutex/semaphore calls are vendor; the brightness arithmetic and
 * the channel-write command assembly are VanMoof. MMIO/RAM accesses are done
 * verbatim against the device addresses (volatile literal-address casts).
 */

#include "frontlight.h"

/* frontlight_pwm_set_duty — program the LED PWM duty on the LPC SCTimer.
 *
 * OEM disassembly (0x0000040c..0x0000047a):
 *
 * The active SCT event index is held at FL_SCT_EVENT_IDX; reads the event's
 * control word (FL_SCT_BASE + idx*8 + 0x304, low nibble = match-state) and state
 * word (+0x30c, low nibble = output channel), then loads the full-scale reload
 * value from the SCT match table at FL_SCT_BASE + (match_state + 0x40)*4.
 *
 * For pct==100 it nudges the reload by +2 or -1 per the SCT direction bit
 * (FL_SCT_BASE+4, bit 27 via the <<0x1b sign test). Otherwise duty =
 * (uint16_t)((pct * fullscale) / 100), via a 64-bit multiply/divide. The new
 * duty is written to MATCHREL (FL_SCT_BASE + chan*4 + 0x100) and its reload
 * counterpart (+0x200), bracketed by setting/clearing the match-buffer guard
 * bit (FL_SCT_BASE+4 |= 4 / &= ~4). (Same shape as elock lock_motor_pwm_set_duty.)
 */
void frontlight_pwm_set_duty(unsigned int pct)
{
    uint32_t ev_ctrl   = *(volatile uint32_t *)(*(uint32_t *)FL_SCT_EVENT_IDX * 8u + (FL_SCT_BASE + 0x304u));
    uint32_t ev_state  = *(volatile uint32_t *)(*(uint32_t *)FL_SCT_EVENT_IDX * 8u + (FL_SCT_BASE + 0x30cu));
    uint32_t fullscale = *(volatile uint32_t *)(FL_SCT_BASE + ((ev_ctrl & 0xfu) + 0x40u) * 4u);
    uint16_t duty;

    if (pct == 100u) {
        short cur = (short)fullscale;
        if (((int)(*(volatile uint32_t *)(FL_SCT_BASE + 4u)) << 0x1b) < 0)
            duty = (uint16_t)(cur - 1);
        else
            duty = (uint16_t)(cur + 2);
    } else {
        uint64_t prod = (uint64_t)pct * (uint64_t)fullscale;
        duty = (uint16_t)(prod / 100u);
    }

    *(volatile uint32_t *)(FL_SCT_BASE + 4u) |= 4u;
    {
        uint32_t chan = (ev_state & 0xfu) * 4u;
        *(volatile uint32_t *)(chan + (FL_SCT_BASE + 0x100u)) = duty;
        *(volatile uint32_t *)(chan + (FL_SCT_BASE + 0x200u)) = duty;
    }
    *(volatile uint32_t *)(FL_SCT_BASE + 4u) &= ~4u;
}

/* frontlight_led_channel_write — core LED-driver primitive: stage a per-channel
 * transfer command and enable the SCTimer output for the channel.
 *
 * OEM disassembly (0x000027f8..0x00002882):
 *
 * Builds a 2-byte command record { channel<<1, (uint8_t)data } and an arg blob
 * { channel, extra } on the stack. Takes the driver mutex (cb[0xe], wait
 * forever); on success, if the SCTimer base (cb[0]) is set and the driver
 * callback slot (cb[7]) is not already FL_LED_CALLBACK, it programs the transfer
 * descriptor (cb[1]=&record, cb[2]=&blob, cb[3]=cb[4]=cb[6]=2, cb[0xb]=0x100000,
 * (uint8_t)cb[5]=0), installs the callback (cb[7]=FL_LED_CALLBACK), enables the
 * SCTimer outputs (base+0xe00 |= 0x30000, +0xe04 |= 3, +0xe10 |= 0xc), then
 * takes the transfer-done semaphore (cb[0xf], wait forever) — returning early if
 * that take fails. Finally it releases the driver mutex (cb[0xe]).
 */
void frontlight_led_channel_write(int channel, unsigned int data, unsigned int extra)
{
    int *cb = (int *)FL_LED_CB;
    struct { uint8_t idx; uint8_t data; } rec;             /* cb[1] target (2 bytes) */
    int blob[2];                                           /* cb[2] target */

    rec.idx  = (uint8_t)(channel << 1);
    rec.data = (uint8_t)data;
    blob[0]  = channel;
    blob[1]  = (int)extra;

    if (rtos_sem_take((void *)cb[0xe], 0xffffffffu) == 1) {
        int base = cb[0];
        if (base != 0 && cb[7] != (int)FL_LED_CALLBACK) {
            int sem2;
            cb[1] = (int)&rec;
            cb[2] = (int)blob;
            cb[6] = 2;
            cb[3] = 2;
            cb[4] = 2;
            cb[0xb] = 0x100000;
            *(uint8_t *)(cb + 5) = 0;
            cb[7] = (int)FL_LED_CALLBACK;
            sem2 = cb[0xf];
            *(volatile uint32_t *)(base + 0xe00) |= 0x30000u;
            *(volatile uint32_t *)(base + 0xe04) |= 0x3u;
            *(volatile uint32_t *)(base + 0xe10) |= 0xcu;
            if (rtos_sem_take((void *)sem2, 0xffffffffu) != 1)
                return;
        }
        rtos_sem_give((void *)cb[0xe], 0);
    }
}

/* frontlight_led_set_brightness — set one LED channel's brightness.
 *
 * OEM disassembly (0x0000582e..0x00005834):
 *
 * Tail-call: computes the LED command index (channel*4 + 0x21) and forwards the
 * brightness byte to frontlight_led_channel_write as its data argument.
 */
void frontlight_led_set_brightness(unsigned char channel, unsigned int brightness)
{
    frontlight_led_channel_write((unsigned char)(channel * 4 + 0x21), brightness, 0);
}
