/*
 * led.c — rear LED driver primitives (per-channel write, brightness).
 *
 * VanMoof S5/A5 rear LED light controller. Translated from the OEM image
 * (NXP LPC546xx Cortex-M4F, image base 0x0). The SCTimer/PWM register pokes and
 * the FreeRTOS mutex/semaphore calls are vendor; the channel-write command
 * assembly is VanMoof. (Structurally == frontlight led.c; no separate
 * pwm_set_duty in the rearlight.)
 */

#include "rearlight.h"

/* rearlight_led_channel_write — core LED-driver primitive: stage a per-channel
 * transfer command and enable the SCTimer output for the channel.
 *
 * OEM disassembly (0x00002698..0x00002722):
 *
 * Builds a 2-byte command record { channel<<1, (uint8_t)data } and an arg blob
 * { channel, arg } on the stack. Takes the driver mutex (cb[0xe], wait forever);
 * on success, if the SCTimer base (cb[0]) is set and the callback slot (cb[7])
 * is not already RL_LED_CALLBACK, it programs the transfer descriptor
 * (cb[1]=&record, cb[2]=&blob, cb[3]=cb[4]=cb[6]=2, cb[0xb]=0x100000,
 * (uint8_t)cb[5]=0), installs the callback (cb[7]=RL_LED_CALLBACK), enables the
 * SCTimer outputs (base+0xe00 |= 0x30000, +0xe04 |= 3, +0xe10 |= 0xc), then
 * takes the transfer-done semaphore (cb[0xf]) — returning early if that fails.
 * Finally it releases the driver mutex. (== frontlight_led_channel_write.)
 */
void rearlight_led_channel_write(uint32_t channel, uint32_t data)
{
    uint32_t *cb = (uint32_t *)RL_LED_CB;
    struct { uint8_t cmd0; uint8_t cmd1; } rec; /* cb[1] target (2 bytes) */
    uint32_t blob[2];                           /* cb[2] target {channel, data} */

    rec.cmd0 = (uint8_t)(channel << 1);
    rec.cmd1 = (uint8_t)data;
    blob[0]  = channel;
    blob[1]  = data;

    if (rtos_sem_take(cb[0xe], 0xffffffffu) == 1) {
        uint32_t base = cb[0];
        if (base != 0 && cb[7] != RL_LED_CALLBACK) {
            uint32_t done_sem;
            cb[1]   = (uint32_t)&rec;
            cb[2]   = (uint32_t)blob;
            cb[6]   = 2;
            cb[3]   = 2;
            cb[4]   = 2;
            cb[0xb] = 0x100000;
            *(uint8_t *)(cb + 5) = 0;
            cb[7]   = RL_LED_CALLBACK;
            done_sem = cb[0xf];
            *(volatile uint32_t *)(base + 0xe00) |= 0x30000u;
            *(volatile uint32_t *)(base + 0xe04) |= 0x3u;
            *(volatile uint32_t *)(base + 0xe10) |= 0xcu;
            if (rtos_sem_take(done_sem, 0xffffffffu) != 1)
                return;
        }
        rtos_sem_give(cb[0xe], 0);
    }
}

/* rearlight_led_set_brightness — set one LED channel's brightness.
 *
 * OEM disassembly (0x00005318..0x0000531e):
 *
 * Tail-call: computes the LED command index (channel*4 + 0x21) and forwards the
 * brightness byte to rearlight_led_channel_write as its data argument.
 */
void rearlight_led_set_brightness(uint8_t channel, uint8_t brightness)
{
    rearlight_led_channel_write((uint8_t)((channel << 2) + 0x21), brightness);
}
