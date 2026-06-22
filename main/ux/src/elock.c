/*
 * elock.c - reconstructed VanMoof S5 i.MX8 `ux` service ElockHandler.
 * OEM source: devices/main/ux/src/elock.cpp. Program "ux" (AArch64, base
 * 0x100000). Behaviour-oriented C: the vendor framework (storage_manager,
 * IMQTTClient, the VM bus, the std::function signal/slot list and its
 * nlohmann::json-variant teardown) is modelled through the externs declared in
 * elock.h, not reconstructed. OEM addresses are quoted per function.
 *
 * The four state-setters (set/store/software_lock and the success path of
 * requestUnlock) share one body in the binary: write the state byte at
 * self->sw_state, mark it valid, persist it through storage_manager, log a
 * "Failed to store ..." warning on failure, then publish "ux/lock/info/state".
 * The large repeated blocks in the decompiler output are the inlined teardown
 * of the json variant handed to storage_store / publish; they carry no app
 * logic and collapse to the modelled calls below.
 */
#include "ux_common.h"
#include "elock.h"

#define ELOCK_CPP "devices/main/ux/src/elock.cpp"

/* Common tail shared by setSoftwareState / softwareLock / storeSoftwareState
 * and the success path of requestUnlock: log, persist, publish. */
static void elock_apply_software_state(ElockHandler *self, uint8_t state)
{
    /* FUN_0017e900 + common_logf line 0x3a */
    common_logf(ELOCK_CPP, 0x3a, LOG_WARN,
                "Setting elock software state to: %s",
                elock_state_to_string(state));

    self->sw_state = state;
    self->sw_state_valid = 1;

    /* storage_manager::store (FUN_001192a0). */
    if (elock_storage_store(self->storage_key, state) == 0) {
        common_logf(ELOCK_CPP, 0x3d, LOG_DEBUG,
                    "Failed to store elock software state");
    }

    /* publisher->publish("ux/lock/info/state", state, qos=1, retain=1) */
    elock_mqtt_publish_state(self->publisher, ELOCK_TOPIC_STATE, state);
}

/* ElockHandler::setSoftwareState(uint8 state) - OEM 0x15fb20.
 * Generic parametrized setter. */
void elock_set_software_state(ElockHandler *self, uint8_t state)
{
    elock_apply_software_state(self, state);
}

/* ElockHandler::softwareLock() - OEM 0x163340.
 * Periodic 5000ms timer body (armed in elock_init). Forces software LOCKED. */
void elock_software_lock(ElockHandler *self)
{
    /* OEM writes 0x101 at self+0xb2: sw_state=1, sw_state_valid=1. */
    self->sw_state_valid = 1;
    elock_apply_software_state(self, ELOCK_STATE_LOCKED);
}

/* ElockHandler::storeSoftwareState(state) - OEM 0x163ad0.
 * Specialized variant (OEM binds state=1) of setSoftwareState. */
void elock_store_software_state(ElockHandler *self, uint8_t state)
{
    elock_apply_software_state(self, state);
}

/* ElockHandler::requestUnlock() - OEM 0x1602f0.
 * Drives the e-lock motor to unlock, transitions software state to UNLOCKING,
 * persists + publishes, then arms the auto-relock timeout. */
void elock_request_unlock(ElockHandler *self)
{
    char rc;

    common_logf(ELOCK_CPP, 0x5e, LOG_WARN, "Request to unlock");

    /* VM call: elock motor unlock (self+0x90 ctx, self+0x70 arg). 0 == ok. */
    rc = elock_vm_request_unlock(self->elock_vm, self->ride_mgr);
    if (rc != 0) {
        common_logf(ELOCK_CPP, 0x62, LOG_DEBUG,
                    "Failed to request the e-lock to unlock");
        return;
    }

    /* OEM writes 0x103 at self+0xb2: sw_state=UNLOCKING, sw_state_valid=1.
     * Same log/persist/publish tail as the shared setter. */
    common_logf(ELOCK_CPP, 0x3a, LOG_WARN,
                "Setting elock software state to: %s",
                elock_state_to_string(ELOCK_STATE_UNLOCKING));
    self->sw_state = ELOCK_STATE_UNLOCKING;
    self->sw_state_valid = 1;

    if (elock_storage_store(self->storage_key, ELOCK_STATE_UNLOCKING) == 0) {
        common_logf(ELOCK_CPP, 0x3d, LOG_DEBUG,
                    "Failed to store elock software state");
    }
    elock_mqtt_publish_state(self->publisher, ELOCK_TOPIC_STATE,
                             ELOCK_STATE_UNLOCKING);

    /* The std::string built from DAT_001ff8f0 + FUN_001816f0 is a vendor
     * metrics/event emit (theft-related); modelled as the clear below. */
    self->mqtt_unlock_seen = 0;

    /* Arm the auto-relock oneshot for relock_timeout_s seconds. */
    elock_relock_arm(self->relock_timer, self->relock_timeout_s * 1000);
}

/* ElockHandler::requestKickUnlock() - OEM 0x162b80.
 * Fan-out of the kick-to-unlock gesture to subscribers. */
void elock_request_kick_unlock(ElockHandler *self)
{
    common_logf(ELOCK_CPP, 0x8a, LOG_WARN, "Elock kick unlock request");
    elock_emit(self, ELOCK_EV_KICK_UNLOCK);
}

/* ElockHandler::onStateReport(report) - OEM 0x162d10.
 * VM-call callback: the e-lock motor reports its physical state. Dedupes
 * against the last report, emits locked/unlocked events, and detects a forced
 * (unexpected) unlock. */
void elock_on_state_report(ElockHandler *self, void *vm_meta, const char *report)
{
    char state;

    (void)vm_meta;
    state = elock_report_state(report);

    if (self->phys_seen == 0) {
        /* First report: remember whether a software LOCK was pending so a
         * boot-time forced unlock can be flagged (self+0xdb). */
        char forced = 0;
        if (self->sw_state_valid != 0)
            forced = (self->sw_state == ELOCK_STATE_LOCKED);
        self->forced_at_boot = (uint8_t)forced;
    } else if (self->phys_state == state) {
        /* Unchanged since last report: nothing to do. */
        return;
    }

    common_logf(ELOCK_CPP, 0x96, LOG_WARN, "ELock is reporting: %s",
                elock_state_to_string(state));

    state = elock_report_state(report);
    if (state == ELOCK_STATE_LOCKED) {              /* physical LOCKED */
        elock_emit(self, ELOCK_EV_LOCKED);
        elock_bike_on_locked(self->bike_mgr);       /* bike vtable+0x18 */
        state = elock_report_state(report);
    } else if (state == ELOCK_STATE_UNLOCKING) {    /* code 3 == UNLOCKED */
        elock_emit(self, ELOCK_EV_UNLOCKED);

        /* bike-state: clear the locked indication if it wants to. */
        if (elock_bike_should_clear(self->bike_mgr) != 0) {  /* vtable+0x28 */
            elock_bike_clear(self->bike_mgr);                /* vtable+0x10 */
            elock_emit(self, ELOCK_EV_UNLOCKED);
        }
        if (elock_bike_should_clear(self->bike_mgr) != 0)
            elock_bike_clear(self->bike_mgr);

        /* Forced unlock: a software LOCK was in effect but the motor opened. */
        if (self->sw_state_valid != 0 && self->sw_state == ELOCK_STATE_LOCKED) {
            common_logf(ELOCK_CPP, 0xae, LOG_DEBUG,
                        "Elock has been forced unlocked");
            /* vendor theft/metrics emit (DAT_001ff8f0 + FUN_00181930). */
            elock_emit(self, ELOCK_EV_FORCED_UNLOCK);
        }
        state = elock_report_state(report);
    }

    self->phys_state = (uint8_t)state;
    self->phys_seen = 1;
}

/* ElockHandler::isUnlocked() - OEM 0x15f9a0.
 * Predicate over the cached state. */
bool elock_is_unlocked(ElockHandler *self)
{
    /* `self` is the strategy wrapper; *self dereferences to the handler. */
    ElockHandler *h = *(ElockHandler **)self;
    bool unlocked;

    if (h->phys_seen != 0 && h->phys_state == ELOCK_STATE_LOCKED)
        return false;

    unlocked = false;
    if (h->sw_state_valid != 0)
        unlocked = (h->sw_state == ELOCK_STATE_LOCKED);
    return unlocked;
}

/* ElockHandler ctor / init - OEM 0x164bd0.
 * Builds the handler (vtable PTR_FUN_001fa438), registers three VM-call
 * handlers on the bike bus (onStateReport + two kick-unlock wrappers), loads
 * the persisted software state from storage_manager (key self->storage_key),
 * defaulting to UNLOCKED if absent/invalid, publishes the initial
 * "ux/lock/info/state", and arms three periodic timers on the scheduler:
 *   5000ms -> softwareLock, 1000ms -> FUN_00164290, 1000ms -> FUN_00160e80.
 * Only the VanMoof-authored bring-up is modelled; the std::function/VM-call
 * registration plumbing is vendor. */
void elock_init(ElockHandler *self, void *lock_mgr, void *ride_mgr,
                void *storage_key, void *scheduler)
{
    uint8_t state;

    self->ride_mgr = ride_mgr;
    self->lock_mgr = lock_mgr;
    self->storage_key = storage_key;
    self->phys_state = 0;
    self->phys_seen = 0;
    self->sw_state = 0;
    self->sw_state_valid = 0;
    self->forced_at_boot = 0;
    self->mqtt_unlock_seen = 0;

    /* Register the three VM-call handlers on the bike bus (vendor wrappers
     * around elock_on_state_report / elock_request_kick_unlock). */

    /* Load persisted software state; default to UNLOCKED on miss/invalid.
     * The OEM guard `2 < (state - 5)` rejects anything outside {5,6,7}. */
    if (!elock_storage_load(self->storage_key, &state) ||
        (uint8_t)(state - 5) > 2) {
        common_logf(ELOCK_CPP, 0x2d, LOG_INFO,
                    "No elock state is retrived, dafaulting to unlocked state");
        state = ELOCK_STATE_UNLOCKED;
        if (elock_storage_store(self->storage_key, state) == 0) {
            common_logf(ELOCK_CPP, 0x30, LOG_DEBUG,
                        "Failed to store elock's default state in storage mannager");
        }
    }
    self->sw_state = state;
    self->sw_state_valid = 1;

    common_logf(ELOCK_CPP, 0x34, LOG_WARN,
                "Initializing elock software state to %s",
                elock_state_to_string(state));
    elock_mqtt_publish_state(self->publisher, ELOCK_TOPIC_STATE, state);

    /* Periodic timers via scheduler vtable+0x10. */
    (void)scheduler;
}
