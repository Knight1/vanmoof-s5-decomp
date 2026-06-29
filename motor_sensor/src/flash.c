/*
 * flash.c — VanMoof S5 motor_sensor: flash sector log + watchdog timer.
 *
 * The flash storage path (CAN-proxied erase/program in 512-byte sectors) used
 * for the persistent NVM calibration record, plus the watchdog timer creator.
 * Translated from the OEM image (LPC546xx-class, base 0x0); the flash command
 * primitives (can_tx_dispatch/poke) and the FreeRTOS timer create are vendor.
 *
 * OEM: flash_write_sector 0x2724, flash_erase_and_program_sector 0x5882,
 * watchdog_timer_start 0x2d14.
 */

#include "motor_sensor.h"

extern const char     *g_wdt_timer_name;   /* **0x2d58 */
extern volatile uint32_t g_wdt_feed_flag[2]; /* 0x20000788 */
extern void wdt_feed_callback(void *t);     /* 0x3914 (interior of the init fn) */

/* ------------------------------------------------------------------- 0x2724 */

/* Write up to 512 bytes into the in-memory page buffer; on a final flush
 * (cur_sector==0xFFFF, offset==0x73fc) stamp the header, issue the CAN-proxied
 * erase+program (cmd 0xf200), append the page tail, and verify/advance.
 * Returns 0 ok / 1 state-error / 2 range-overflow. */
uint32_t flash_write_sector(flash_obj_t *obj, uint32_t off, const void *src, uint32_t len)
{
    if (obj->initialized == 0)
        return 1;
    if (off > 0x73fcu || off + len > 0x73fcu + 4u)
        return 2;

    if (obj->cur_sector == 0xffffu && off == 0x73fcu) {
        /* final flush */
        uint8_t *page = (uint8_t *)obj->page_buf;
        mem_set(page, 0xff, 0x200);
        *(uint32_t *)(page + 0) = 0x00010039u;        /* sector-index header */
        can_tx_dispatch(obj, 0xf200u, 0x200, NULL);    /* erase+program */
        mem_cpy(page + 0x1fc, src, len);               /* tail data */
        can_tx_poke(obj, 0xf200u);
        return (uint32_t)flash_page_verify_and_advance(obj);
    }

    if (obj->cur_sector == (uint16_t)(off >> 9)) {     /* within buffered sector */
        mem_cpy((uint8_t *)obj->page_buf + (off & 0x1ffu), src, len);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------- 0x5882 */

/* Erase a run of up to 58 sectors and reinit the page buffer. Each sector's
 * physical byte address is (sector + 0x40) * 0x200. Returns 0 / -1. */
int32_t flash_erase_and_program_sector(flash_ctx_t *ctx, uint32_t p2, uint16_t *spec, uint32_t p4)
{
    flash_obj_t *obj = ctx->obj;
    uint16_t start = spec[0];
    uint16_t count = spec[1];
    uint16_t s;
    (void)p2; (void)p4;

    ctx->status = 0;
    ctx->flag15 = 0;
    if (obj == NULL || obj->initialized == 0)
        return -1;

    flash_page_verify_and_advance(obj);                /* flush in-progress page */

    for (s = start; s < start + count; s++) {
        if (s >= 0x3au)                                /* max sector = 0x39 */
            return -1;
        can_tx_poke(obj, (uint32_t)(s + 0x40u) * 0x200u);  /* sector erase */
    }

    mem_set(obj->page_buf, 0xff, 0x200);
    obj->cur_sector   = start;
    obj->sectors_left = (int16_t)count;
    ctx->result_start = start;
    ctx->in_progress  = 0;
    return 0;
}

/* ------------------------------------------------------------------- 0x2d14 */

/* Watchdog timer creator (NOT the feed callback — that is the interior fn at
 * 0x3914). Tries to (re)create the 5 s auto-reload timer; only if creation
 * FAILS does it service the WWDT directly (0xAA/0x55 under PRIMASK). */
void watchdog_timer_start(void *xTimer, uint32_t p2, uint32_t p3)
{
    void *h;
    (void)xTimer; (void)p2; (void)p3;

    h = xTimerCreate_wrap((void *)g_wdt_timer_name, 0x1388, 1, NULL, wdt_feed_callback);
    if (h != NULL)
        return;                                        /* created OK */

    if (g_wdt_feed_flag[0] == g_wdt_feed_flag[1]) {     /* feed-needed flag pair */
        uint32_t pm = port_set_interrupt_mask();
        g_wdt_feed_flag[0] = 0;
        MMIO32(WWDT_FEED) = 0xaa;                       /* NXP WWDT unlock */
        MMIO32(WWDT_FEED) = 0x55;
        port_clear_interrupt_mask(pm);
    }
}
