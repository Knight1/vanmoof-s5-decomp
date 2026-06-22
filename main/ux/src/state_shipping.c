/*
 * state_shipping.c -- VanMoof S5 i.MX8 `ux` service: ShippingStrategy.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   state_shipping_strategy_ctor  0x134d60
 *
 * On entering the Shipping state the strategy announces the 'bike_shipping'
 * sound (the publish is built directly on the sound manager db_c0 via the
 * OEM publish helper 0x1996e0 with the non-looping flag, priority 100).
 */
#include "ux_common.h"
#include "state_shipping.h"

/*
 * state_shipping_strategy_ctor -- 0x134d60
 * vtable &DAT_001f9fd0.
 */
void state_shipping_strategy_ctor(state_shipping_strategy *self,
                                  int state, ux_service *svc)
{
    self->vtable = (const void *)0;   /* &DAT_001f9fd0 (set by OEM) */
    self->state  = state;
    self->svc    = svc;

    /* Announce the shipping sound on entry (FUN_001996e0, name len 0xd). */
    ux_sound_play(svc, "bike_shipping");
}
