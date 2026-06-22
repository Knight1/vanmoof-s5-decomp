/*
 * state_updating.c -- VanMoof S5 i.MX8 `ux` service: UpdatingStrategy.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   state_updating_strategy_ctor  0x134ea0
 *
 * On entering the Updating (OTA) state the strategy:
 *   - announces the 'firmware_update_loop' sound (priority 0x1e, looping),
 *   - sets the light ring to mode 3,
 *   - uploads the 0xb4 progress animation frames (timed; logs the duration),
 *   - starts pattern 0xb4 looping and enables it.
 */
#include "ux_common.h"
#include "state_updating.h"

/*
 * state_updating_strategy_ctor -- 0x134ea0
 * obj size 0x18, vtable &DAT_001fa000.
 */
void state_updating_strategy_ctor(state_updating_strategy *self,
                                  int state, ux_service *svc)
{
    void *light;
    long  t0, t1;

    self->vtable = (const void *)0;   /* &DAT_001fa000 (set by OEM) */
    self->state  = state;
    self->svc    = svc;

    /* FUN_0013db60(svc,0): touch the OTA/update sub-manager (svc+0x4b8). */
    (void)ux_ride(svc);   /* db-thunk accessor; result unused on entry */
    /* FUN_0015d750(): pre-roll the animation pipeline (VENDOR helper). */

    /* Announce the firmware-update-loop sound: looping, priority 0x1e. */
    ux_sound_play(svc, "firmware_update_loop");

    /* Light ring to mode 3 (FUN_00147f00 -> pattern 0x1f5). */
    light = ux_sound(svc);            /* db_80 light manager */
    ux_light_set_mode(svc, 3);
    (void)light;

    /* Upload the 0xb4 progress animation, timing the upload. */
    t0 = ux_clock_ns();
    light_upload_progress_animation(ux_sound(svc), 0xb4, 0x3fd);
    t1 = ux_clock_ns();
    common_logf("devices/main/ux/src/state_updating_strategy.cpp", 0x13, LOG_WARN,
                "Animation uploaded in %f seconds",
                (double)(t1 - t0) / 1000000000.0);

    /* Start pattern 0xb4 looping, then enable it. */
    ux_light_pattern(svc, 0xb4, 3);
    light_pattern_enable(ux_sound(svc), true);
}
