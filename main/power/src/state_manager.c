/*
 * state_manager.c — StateManager + IStateTransitions dispatch (reconstructed)
 *
 * OEM: /usr/bin/power, devices/main/power/src/state_manager.cpp.
 *   StateManager::ChangeState     0x11acb0
 *   StateManager::OnStateRequest  0x11b520
 *   StateManager::StateName       0x142670
 *   sm_subscribe                  0x11a7e0
 *   on_state_set                  0x11c3a0  (power/state/set      -> OnStateRequest)
 *   on_extend_timeout             0x11c320  (power/state/extend_timeout)
 *
 * Faithful translation of the decompiled logic. The state/pending fields are
 * accessed with C11 acquire/release atomics, matching the OEM `ldar`/`stlr`.
 * The large inlined blocks in ChangeState/OnStateRequest are the destructor of
 * the OD-publish return value (an STL vector/string discarded by the type tag
 * in the call's status byte); that is framework/STL and is folded into the
 * od_pub_int() helper here. Topic strings and status codes are verbatim.
 */
#include <stdatomic.h>
#include "state_manager.h"

#define SM_FILE "devices/main/power/src/state_manager.cpp"

/*
 * Pure-`ret` default transition stubs. The OEM stores these at 0x11c410 (slot
 * +0x10) ... 0x11c470 (slot +0x40), one per state; an un-overridden slot points
 * here and the dispatcher skips it (`if (handler != default) handler(this)`).
 * ALARM (+0x38) has no skip check in the binary and is always invoked.
 */
extern void sm_default_on_shipping(void *self);    /* 0x11c410 */
extern void sm_default_on_standby(void *self);     /* 0x11c420 */
extern void sm_default_on_operational(void *self); /* 0x11c430 */
extern void sm_default_on_charging(void *self);    /* 0x11c440 */
extern void sm_default_on_updating(void *self);    /* 0x11c450 */
extern void sm_default_on_maintenance(void *self); /* 0x11c470 */

/*
 * OEM 0x142670 — StateManager::StateName(out, state).
 *
 * Returns the textual name of `state` into the caller's std::string `out`.
 * The C reconstruction returns a const char* through *out; all the names fit
 * the std::string SSO buffer in the binary (no heap), so this is allocation
 * free in both forms. INVALID is the default for any out-of-range value.
 */
void StateManager_StateName(void *out, int state)
{
    const char *name;

    switch (state) {
    case ST_SHIPPING:    name = "SHIPPING";    break; /* 0x474e495050494853 */
    case ST_STANDBY:     name = "STANDBY";     break; /* DAT_00160d50       */
    case ST_OPERATIONAL: name = "OPERATIONAL"; break; /* DAT_00160d58       */
    case ST_CHARGING:    name = "CHARGING";    break; /* 0x474e494752414843 */
    case ST_UPDATING:    name = "UPDATING";    break; /* 0x474e495441445055 */
    case ST_ALARM:       name = "ALARM";       break; /* DAT_00160d68       */
    case ST_MAINTENANCE: name = "MAINTENANCE"; break; /* DAT_00160d70       */
    case ST_INVALID:
    default:             name = "INVALID";     break; /* DAT_00160d80       */
    }

    *(const char **)out = name;
}

/*
 * OEM 0x11acb0 — StateManager::ChangeState(this, state).
 *
 * Commit a new power_state. Logs the target, refuses INVALID, then:
 *   1. publishes the new state to `power/state`   (retained),
 *   2. atomically stores `state`,
 *   3. if the controller is busy, fires its on_state_changed() hook,
 *   4. publishes `power/state/status` = 0 (cleared) (not retained),
 *   5. atomically clears `pending`.
 */
void StateManager_ChangeState(struct StateManager *sm, int state)
{
    const char *name;

    StateManager_StateName(&name, state);
    common_logf(SM_FILE, 0x71, LOG_INFO,
                "StateManager::ChangeState(): %s", name);

    if (state == ST_INVALID) {
        common_logf(SM_FILE, 0x73, LOG_WARN,
                    "Someone's trying to go into INVALID state");
        return;
    }

    /* (1) publish new state value to power/state, qos 5, retained */
    od_pub_int(sm->ipc, "power/state", state, 5, 1);

    /* (2) commit the active state (release store) */
    atomic_store_explicit((_Atomic int32_t *)&sm->state, state,
                          memory_order_release);

    /* (3) controller hook: if busy(), notify of the committed change */
    if (sm->controller->is_busy(sm->controller))
        sm->controller->on_state_changed(sm->controller);

    /* (4) clear the request status: power/state/status = 0, not retained */
    od_pub_int(sm->ipc, "power/state/status", 0, 5, 0);

    /* (5) clear the in-flight request (release store) */
    atomic_store_explicit((_Atomic int32_t *)&sm->pending, 0,
                          memory_order_release);
}

/*
 * Dispatch the matching IStateTransitions slot for `state`. Each slot is
 * skipped when it still points at its pure-`ret` default; ALARM (+0x38) is
 * always invoked. Unhandled/invalid requests are logged at level ERR.
 * Mirrors the OEM switch at 0x11b5a8 / 0x11b784.
 */
static void sm_dispatch(struct StateManager *sm, int state)
{
    struct IStateTransitions *t = sm->transitions;

    switch (state) {
    case ST_SHIPPING:
        if (t->on_shipping != sm_default_on_shipping)
            t->on_shipping(sm);
        break;
    case ST_STANDBY:
        if (t->on_standby != sm_default_on_standby)
            t->on_standby(sm);
        break;
    case ST_OPERATIONAL:
        if (t->on_operational != sm_default_on_operational)
            t->on_operational(sm);
        break;
    case ST_CHARGING:
        if (t->on_charging != sm_default_on_charging)
            t->on_charging(sm);
        break;
    case ST_UPDATING:
        if (t->on_updating != sm_default_on_updating)
            t->on_updating(sm);
        break;
    case ST_ALARM:
        t->on_alarm(sm);          /* no default-skip in the binary */
        break;
    case ST_MAINTENANCE:
        if (t->on_maintenance != sm_default_on_maintenance)
            t->on_maintenance(sm);
        break;
    default:
        common_logf(SM_FILE, 0x59, LOG_ERR,
                    "Unhandled or invalid state request.");
        if (state == ST_INVALID)
            return;             /* OEM: bail before the OK log */
        break;
    }

    common_logf(SM_FILE, 0x5e, LOG_INFO, "StateManager::OnStateRequest(): OK");
}

/*
 * OEM 0x11b520 — StateManager::OnStateRequest(this, state, internal).
 *
 * `internal` is the OEM third arg (a byte). The on_state_set handler passes 0
 * for an external request from `power/state/set`; internal/forced callers pass
 * a non-zero value to bypass the IS_ACTIVE/RETRY guards. Decision table (status
 * published on `power/state/status`):
 *
 *   current == INVALID                  -> DENIED   (3), no transition
 *   state == current, external          -> IS_ACTIVE(2), no transition
 *   state != current, busy, external    -> RETRY    (1), no transition
 *   otherwise                           -> commit pending=state, dispatch slot
 *
 * On the "force" path (controller not busy) a non-INVALID *external* request
 * additionally re-publishes the live state via republish_state("power/state").
 */
void StateManager_OnStateRequest(struct StateManager *sm, int state, bool internal)
{
    int32_t current;

    current = atomic_load_explicit((_Atomic int32_t *)&sm->state,
                                   memory_order_acquire);

    /* --- current state is INVALID: reject outright ------------------- */
    if (current == ST_INVALID) {
        common_logf(SM_FILE, 0x2c, LOG_WARN,
                    "StateManager::OnStateRequest(): DENIED");
        od_pub_int(sm->ipc, "power/state/status", SM_DENIED, 5, 0);
        return;
    }

    if (state == current) {
        /* already in the requested state */
        if (!internal) {
            common_logf(SM_FILE, 0x31, LOG_INFO,
                        "StateManager::OnStateRequest(): IS_ACTIVE");
            od_pub_int(sm->ipc, "power/state/status", SM_IS_ACTIVE, 5, 0);
            return;
        }
        /* internal re-request: touch the busy gate, then re-dispatch */
        sm->controller->is_busy(sm->controller);
        atomic_store_explicit((_Atomic int32_t *)&sm->pending, state,
                              memory_order_release);
        sm_dispatch(sm, state);
        return;
    }

    /* --- changing state: consult the busy gate ----------------------- */
    if (sm->controller->is_busy(sm->controller)) {
        /* a transition is in progress */
        if (!internal) {
            common_logf(SM_FILE, 0x35, LOG_WARN,
                        "StateManager::OnStateRequest(): RETRY");
            od_pub_int(sm->ipc, "power/state/status", SM_RETRY, 5, 0);
            return;
        }
        /* internal/forced request overrides the lock: commit + dispatch */
        atomic_store_explicit((_Atomic int32_t *)&sm->pending, state,
                              memory_order_release);
        sm_dispatch(sm, state);
        return;
    }

    /* --- not busy: force the request through ------------------------- */
    atomic_store_explicit((_Atomic int32_t *)&sm->pending, state,
                          memory_order_release);

    /* a fresh, non-INVALID external request re-emits the current state.
     * OEM guard: (state != 0) & (internal ^ 1)  ==  state != INVALID && !internal */
    if (state != ST_INVALID && !internal)
        sm->controller->republish_state(sm->controller, "power/state");

    sm_dispatch(sm, state);
}

/* Decode an int from an MQTT/OD message body (framework; OEM 0x11c6b0/0x11c480).
 * Defaults *out to 0 when the body is empty/unparseable. */
extern void sm_parse_int(const void *msg, int *out);

/*
 * OEM 0x11c3a0 — on_state_set: handler bound to the `power/state/set`
 * subscription (registered by sm_subscribe 0x11a7e0). The bound context's
 * first word is the StateManager*; the payload int is forwarded as an external
 * request (internal = 0 in the OEM call -> the "external set" path).
 */
void sm_on_state_set(struct StateManager **binding, const void *msg)
{
    struct StateManager *sm = binding[0];
    int requested = 0;

    sm_parse_int(msg, &requested);                 /* OEM FUN_0011c6b0 */
    StateManager_OnStateRequest(sm, requested, false);
}

/*
 * OEM 0x11c320 — on_extend_timeout: handler bound to
 * `power/state/extend_timeout`. The bound context's first word is the object
 * whose vtable +0x60 is on_extend_timeout(seconds); the payload int (seconds,
 * default 0) is passed through.
 */
void sm_on_extend_timeout(struct IStateTransitions **binding, const void *msg)
{
    struct IStateTransitions *obj = binding[0]; /* *param_1 -> object         */
    int seconds = 0;

    sm_parse_int(msg, &seconds);                /* OEM FUN_0011c480           */
    obj->on_extend_timeout(obj, seconds);       /* (*obj->vtable[0x60])(seconds) */
}
