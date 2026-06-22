/* state_shipping.h -- ShippingStrategy (UXService UX_SHIPPING strategy).
 *
 * OEM: ctor 0x134d60 (vtable 0x1f9fd0). Entered while the bike is in the
 * shipping (locked-for-transit) state.
 */
#ifndef STATE_SHIPPING_H
#define STATE_SHIPPING_H

#include "ux_common.h"

/* ShippingStrategy object: [0]=vtable, +8=state id, +0x10=svc. */
typedef struct state_shipping_strategy {
    const void *vtable;   /* &DAT_001f9fd0 */
    int         state;    /* UX_SHIPPING */
    ux_service *svc;
} state_shipping_strategy;

void state_shipping_strategy_ctor(state_shipping_strategy *self,
                                  int state, ux_service *svc);

#endif /* STATE_SHIPPING_H */
