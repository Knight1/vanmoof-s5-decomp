/* state_updating.h -- UpdatingStrategy (UXService UX_UPDATING strategy).
 *
 * OEM: ctor 0x134ea0 (obj size 0x18, vtable 0x1fa000). Entered during an OTA
 * firmware update.
 */
#ifndef STATE_UPDATING_H
#define STATE_UPDATING_H

#include "ux_common.h"

/* UpdatingStrategy object: [0]=vtable, +8=state id, +0x10=svc. */
typedef struct state_updating_strategy {
    const void *vtable;   /* &DAT_001fa000 */
    int         state;    /* UX_UPDATING */
    ux_service *svc;
} state_updating_strategy;

/* Light-ring progress-animation helpers (light manager db_80, 0x147*).
 * Modelled at the call site; the std::deque/frame upload glue is VENDOR. */
void light_upload_progress_animation(void *light_mgr, int first_id, int count);
void light_pattern_loop(void *light_mgr, int pattern_id);
void light_pattern_enable(void *light_mgr, bool on);

/* monotonic clock in nanoseconds (OEM 0x10dea0). */
long ux_clock_ns(void);

void state_updating_strategy_ctor(state_updating_strategy *self,
                                  int state, ux_service *svc);

#endif /* STATE_UPDATING_H */
