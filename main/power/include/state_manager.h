/*
 * state_manager.h — StateManager + IStateTransitions (reconstructed)
 *
 * OEM: /usr/bin/power, devices/main/power/src/state_manager.cpp.
 *   StateManager::ChangeState     0x11acb0
 *   StateManager::OnStateRequest  0x11b520
 *   StateManager::StateName       0x142670
 *   sm_subscribe (MQTT wiring)    0x11a7e0  (power/state/set, .../extend_timeout)
 *   on_state_set (sub handler)    0x11c3a0
 *   on_extend_timeout (handler)   0x11c320
 *
 * StateManager owns the power_state FSM. External actors request a state on
 * `power/state/set`; ChangeState drives the transition and publishes the result
 * (`power/state`, `power/state/status`). The seven per-state transition bodies
 * live behind a vtable (IStateTransitions) populated by PowerService — the
 * StateManager only dispatches; it does not implement the transitions.
 */
#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "power_common.h"

/*
 * IStateTransitions — the per-state transition interface. The OEM vtable slots
 * (relative to *StateManager.transitions at +0x00) map 1:1 onto the power_state
 * enum (SHIPPING..MAINTENANCE). PowerService installs its OnShipping/OnStandby/...
 * overrides here; un-overridden slots point at a pure-`ret` default stub
 * (binary: 0x11c410 + 0x10*n) which the dispatcher skips.
 */
struct IStateTransitions {
    void (*reserved0)(void *self);  /* vtable +0x00                    */
    void (*reserved8)(void *self);  /* vtable +0x08                    */
    void (*on_shipping)(void *self);    /* +0x10  ST_SHIPPING    (1)   */
    void (*on_standby)(void *self);     /* +0x18  ST_STANDBY     (2)   */
    void (*on_operational)(void *self); /* +0x20  ST_OPERATIONAL (3)   */
    void (*on_charging)(void *self);    /* +0x28  ST_CHARGING    (4)   */
    void (*on_updating)(void *self);    /* +0x30  ST_UPDATING    (5)   */
    void (*on_alarm)(void *self);       /* +0x38  ST_ALARM       (6)   */
    void (*on_maintenance)(void *self); /* +0x40  ST_MAINTENANCE (7)   */
    void (*on_extend_timeout)(void *self, int seconds); /* +0x60       */
};

/*
 * IStateController — the transition gate / current-state publisher (object at
 * StateManager+0x48). vtable:
 *   +0x10  on_state_changed()  — fired after a state is committed (when busy() true)
 *   +0x20  republish_state(topic) — re-emit current state to `power/state`
 *   +0x28  is_busy()           — bool: a transition is in progress / locked
 */
struct IStateController {
    void (*reserved0)(void *self);
    void (*on_state_changed)(void *self);          /* +0x10 */
    void (*reserved18)(void *self);
    void (*republish_state)(void *self, const char *topic); /* +0x20 */
    bool (*is_busy)(void *self);                   /* +0x28 */
};

/*
 * StateManager object (offsets are the OEM layout used by the three functions).
 * `state`/`pending` are accessed with acquire/release atomics in the binary.
 */
struct StateManager {
    struct IStateTransitions *transitions; /* +0x00 vtable (this == &transitions) */
    uint8_t  _pad08[0x38 - 0x08];
    void    *ipc;                          /* +0x38 OD/MQTT publish client        */
    int32_t  state;                        /* +0x40 current power_state (atomic)  */
    int32_t  pending;                      /* +0x44 in-flight request (atomic)    */
    struct IStateController *controller;   /* +0x48 transition gate / republisher */
};

/* status codes published on `power/state/status` (the request outcome). */
enum sm_status {
    SM_RETRY     = 1,  /* busy: transition blocked, caller should retry */
    SM_IS_ACTIVE = 2,  /* already in the requested state                */
    SM_DENIED    = 3,  /* current state is INVALID — request rejected   */
};

/* OEM 0x142670. Fill *out with the C-string name for `state` (no allocation
 * for the names that fit the std::string SSO buffer). */
void StateManager_StateName(void *out, int state);

/* OEM 0x11acb0. Commit `state`: publish power/state, run the controller
 * hook, then publish power/state/status=0 and clear `pending`. */
void StateManager_ChangeState(struct StateManager *sm, int state);

/* OEM 0x11b520. Handle a state request. `internal`=false for an external
 * request from the `power/state/set` topic (the on_state_set handler passes 0);
 * internal/forced callers pass true to bypass the IS_ACTIVE/RETRY guards.
 * Publishes RETRY/IS_ACTIVE/DENIED on rejection, otherwise dispatches the
 * matching IStateTransitions slot. */
void StateManager_OnStateRequest(struct StateManager *sm, int state, bool internal);

#endif /* STATE_MANAGER_H */
