/* state_maintenance.h -- MaintenanceStrategy (UXService UX_MAINTENANCE strategy).
 *
 * OEM: ctor 0x136a10 (obj size 0x98, vtable 0x1fa150), on_button 0x136890.
 * Entered when the bike is in Maintenance (battery-locked) state.
 */
#ifndef STATE_MAINTENANCE_H
#define STATE_MAINTENANCE_H

#include "ux_common.h"

/* Opaque button-press listener sub-manager embedded in the strategy object at
 * +0x18 (constructed by the OEM std::function/listener-map glue at 0x148fa0 /
 * 0x148dc0 -- VENDOR, modelled here only as a register/notify pair). */
typedef struct elock_button_listener { unsigned char _opaque[40]; } elock_button_listener;
void elock_button_listener_init(elock_button_listener *l, void *bike_mgr);
void elock_button_listener_on_press(elock_button_listener *l,
                                    void (*cb)(void *ctx), void *ctx);

/* MaintenanceStrategy object. strategy[0]=vtable, +8=state id, +0x10=svc,
 * +0x18=button listener (OEM layout). */
typedef struct state_maintenance_strategy {
    const void           *vtable;     /* &DAT_001fa150 */
    int                   state;      /* UX_MAINTENANCE */
    ux_service           *svc;
    elock_button_listener button;     /* +0x18 */
} state_maintenance_strategy;

void state_maintenance_strategy_ctor(state_maintenance_strategy *self,
                                     int state, ux_service *svc);
void state_maintenance_strategy_on_button(state_maintenance_strategy *self);

#endif /* STATE_MAINTENANCE_H */
