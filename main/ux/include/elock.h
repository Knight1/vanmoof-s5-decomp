/*
 * elock.h - module declarations for the reconstructed VanMoof S5 i.MX8 `ux`
 * ElockHandler (devices/main/ux/src/elock.cpp). Included after ux_common.h.
 *
 * The ElockHandler owns the e-lock motor's software/physical state and mirrors
 * it to MQTT ("ux/lock/info/state") and to persistent storage. Software-state
 * codes match the OEM `elock_state_to_string` table (FUN_0017e900):
 *   1 = LOCKED, 3 = UNLOCKING/UNLOCKED, 5 = (default) UNLOCKED.
 */
#ifndef UX_ELOCK_H
#define UX_ELOCK_H

#include "ux_common.h"

/* Software e-lock state codes (FUN_0017e900 string table indices). */
#define ELOCK_STATE_LOCKED     1
#define ELOCK_STATE_UNLOCKING  3
#define ELOCK_STATE_UNLOCKED   5

/* MQTT topic published on every software-state change (DAT_001c52f8, len 0x12). */
#define ELOCK_TOPIC_STATE "ux/lock/info/state"

/*
 * ElockHandler object. Field offsets follow the OEM layout (`self+0x..`) so the
 * reconstructed logic touches the same words as the binary. Only the fields the
 * reconstructed app code reads/writes are named; vendor sub-objects (the VM-bus
 * registration list, the signal/slot deque at +0x38, the mutex at +8) are kept
 * opaque.
 */
typedef struct ElockHandler {
    void   *vtable;            /* +0x00  PTR_FUN_001fa438 */
    uint8_t _pad08[0x38 - 0x08];
    /* +0x38: signal/slot subscriber list (vendor std::function deque) */
    uint8_t _signal_list[0x60 - 0x38];
    void   *lock_mgr;          /* +0x60  lock/elock manager */
    void   *ride_mgr;          /* +0x70  ride manager (VM ctx for unlock cmd) */
    long    relock_timeout_s;  /* +0x78  auto-relock timeout, seconds */
    void   *publisher;         /* +0x88  MQTT publisher (IMQTTClient*) */
    void   *elock_vm;          /* +0x90  e-lock motor VM call context */
    void   *bike_mgr;          /* +0xC0  bike-state manager (event sink, =200) */
    void   *relock_timer;      /* +0xC8  auto-relock oneshot timer */
    /* state block @ +0xB0 */
    uint8_t phys_state;        /* +0xB0  last reported physical state */
    uint8_t phys_seen;         /* +0xB1  a physical report has been received */
    uint8_t sw_state;          /* +0xB2  software state code */
    uint8_t sw_state_valid;    /* +0xB3  a software state has been set */
    uint8_t _padB4[0xB8 - 0xB4];
    void   *storage_key;       /* +0xB8  storage_manager persistence key */
    uint8_t _padC0[0xDA - 0xC0];
    uint8_t mqtt_unlock_seen;  /* +0xDA  cleared on requestUnlock */
    uint8_t forced_at_boot;    /* +0xDB  set if forced-unlock seen on 1st report */
} ElockHandler;

/* --- vendor framework helpers modelled at the call site (not reconstructed) - */

/* FUN_0017e900: e-lock state code -> human string (for logging). */
extern const char *elock_state_to_string(int state);

/* FUN_0017e620: ask the e-lock motor to physically unlock. 0 = ok. */
extern char elock_vm_request_unlock(void *vm_ctx, void *ride_mgr);

/* storage_manager (FUN_001192a0 / FUN_0011ad30): persist/load the state byte
 * as a json variant. store() returns 0 on success; load() returns false if
 * absent/invalid. Modelled as a single byte (the variant the OEM serialises). */
extern char elock_storage_store(void *key, uint8_t state);
extern bool elock_storage_load (void *key, uint8_t *out_state);

/* IMQTTClient::publish(topic,payload,qos=1,retain=1) via vtable+0x20. */
extern void elock_mqtt_publish_state(void *publisher, const char *topic,
                                     uint8_t state);

/* Signal/slot fan-out (FUN_001629f0 + teardown): emit one of the elock events
 * to every subscriber registered on the +0x38 list, under the +8 mutex. The
 * `event` selects which OEM thunk pair (FUN_00161040/60 etc.) was bound. */
enum elock_event {
    ELOCK_EV_LOCKED        = 1,  /* state report == locked   */
    ELOCK_EV_UNLOCKED      = 3,  /* state report == unlocked */
    ELOCK_EV_FORCED_UNLOCK = 4,  /* unexpected/forced unlock */
    ELOCK_EV_KICK_UNLOCK   = 5   /* kick-to-unlock gesture   */
};
extern void elock_emit(ElockHandler *self, enum elock_event event);

/* bike-state manager (self+0xC0) vtable thunks used by onStateReport. */
extern void elock_bike_on_locked   (void *bike_mgr);          /* vtable+0x18 */
extern char elock_bike_should_clear(void *bike_mgr);          /* vtable+0x28 */
extern void elock_bike_clear       (void *bike_mgr);          /* vtable+0x10 */

/* auto-relock oneshot timer (self+0xC8) vtable+0x20: arm for `ms`. */
extern void elock_relock_arm(void *relock_timer, long ms);

/* report passed to onStateReport: first byte is the physical state code. */
extern char elock_report_state(const char *report);

void elock_on_state_report (ElockHandler *self, void *vm_meta, const char *report);
void elock_request_kick_unlock(ElockHandler *self);
void elock_software_lock   (ElockHandler *self);
void elock_set_software_state(ElockHandler *self, uint8_t state);
void elock_store_software_state(ElockHandler *self, uint8_t state);
void elock_request_unlock  (ElockHandler *self);
bool elock_is_unlocked     (ElockHandler *self);

#endif /* UX_ELOCK_H */
