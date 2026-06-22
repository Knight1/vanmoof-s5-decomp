/*
 * reset.c — VanMoof S5 i.MX8 `ux` service: factory-reset-by-button-hold.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   Reset::OnPress    0x178c10
 *   Reset::OnRelease  0x178d70
 * (both recovered from an undisassembled indirect-dispatch gap).
 *
 * The power button long-press drives a reboot/factory-reset: OnPress records
 * the press deadline and schedules a "reboot" action; OnRelease cancels it if
 * the button is released before the hold threshold elapses.
 *
 * The clock source (virtual vt+0x10 = now) and the press-event scheduler /
 * std::function action list are vendor framework — modelled at the call site.
 */
#include "ux_common.h"
#include "reset.h"

/* --- vendor helpers referenced at the call sites (not reconstructed) ------- */
extern long reset_clock_now(void *clock_source);              /* (*vt+0x10)() */
extern void reset_schedule_reboot(void *scheduler);           /* FUN_001996e0 — "Long press reset"/"reboot" @100ms */
extern void reset_cancel(void *scheduler);                    /* FUN_00198750 */

/*
 * Reset::OnPress(this) — 0x178c10.
 *
 * Logs the long-press, records the press deadline (clock now + hold_ms), and
 * appends the "Long press reset"/"reboot" action to the press-event scheduler
 * with a 100 ms debounce.
 */
void reset_on_press(reset_strategy *self)
{
    long now;

    common_logf("devices/main/ux/src/reset.cpp", 0x10, LOG_WARN, "Long press reset");

    now = reset_clock_now(self->vtable);        /* (*vt+0x10)() */
    self->deadline = self->hold_ms + now;       /* this[2] = this[3] + now */

    /* append {"Long press reset" -> "reboot"} to the action vector (100ms). */
    reset_schedule_reboot(self->scheduler);     /* FUN_001996e0(this[1], ...) */
}

/*
 * Reset::OnRelease(this) — 0x178d70.
 *
 * Logs the release; if the elapsed clock value is still below the recorded
 * deadline (this[2]), the long-press has not completed, so the pending reset
 * is cancelled.
 */
void reset_on_release(reset_strategy *self)
{
    long now;

    common_logf("devices/main/ux/src/reset.cpp", 0x1a, LOG_WARN, "Released reset");

    now = reset_clock_now(self->vtable);        /* (*vt+0x10)() */
    if (self->deadline <= now)
        return;

    reset_cancel(self->scheduler);              /* FUN_00198750(this[1]) */
}
