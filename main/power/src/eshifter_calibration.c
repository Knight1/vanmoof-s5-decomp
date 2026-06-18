/*
 * devices/main/power/src/eshifter_calibration.c  (reconstructed)
 *
 * The e-shifter auto-calibration state machine. The power service owns it because
 * calibration may only run while charging (it spins the hub through its gears,
 * which needs mains power) — battery_power_on_sequence kicks it on entry to
 * CHARGING via eshifter_request_calibration().
 *
 * MQTT wiring (set up in the ctor):
 *   subscribe  "eshifter/state"            -> eshifter_on_state_update  (the FSM)
 *   subscribe  "eshifter/last_calibrated"  -> eshifter_on_last_calibrated (restore)
 *   publish    "eshifter/gear/set"         -> command the hub to calibrate
 *   publish    "eshifter/last_calibrated"  -> persist the day calibration finished
 *
 * eshifter_calibration_timeout() (0x11fee0) was MISSED by Ghidra auto-analysis
 * (uncarved gap after charge_counters_reset); it was recovered from the string
 * xref to "Preventing further calibration attempts until next charging".
 *
 * Behaviour-oriented C (per-TU compilable). The nlohmann-json gear extraction,
 * the std::string topic building and the IMQTTClient publish/subscribe are
 * modelled as the helpers below; the OEM addresses are noted per function.
 */
#include <stdint.h>
#include <stdbool.h>

#include "power_common.h"   /* common_logf, LOG_* */

/* ---- framework model (IMQTTClient / clock / json / config override) ------ */
typedef void (*esc_cb)(void *ctx, const void *topic, const void *meta,
                       const void *msg);

void     esc_subscribe(void *ipc, const char *topic, esc_cb cb, void *ctx);
void     esc_unsubscribe(void *ipc, const char *topic);
bool     json_is_number(const void *msg);                 /* message is a bare number */
long     json_as_int(const void *msg);
int      json_get_int(const void *msg, const char *key);  /* msg["current_gear"]       */
void     ipc_publish_calibrate(void *ipc, const char *topic);          /* gear/set cmd */
void     ipc_publish_day(void *ipc, const char *topic, long day);      /* retained     */
uint64_t clock_now_ns(void *clock);                       /* steady clock, ns          */
void     od_override_block_calibration(bool blocked);     /* OEM FUN_0013eaa0 (config)  */

/* ---- the e-shifter calibration object ------------------------------------ */
enum esc_state {                 /* obj+0x18 */
    ESC_IDLE        = 0,
    ESC_REQUEST     = 1,         /* a calibration is queued; send it on next update  */
    ESC_SENT        = 2,         /* "eshifter/gear/set" published; waiting for gear 0 */
    ESC_IN_PROGRESS = 3,         /* hub is cycling gears; done when it returns to 1   */
};

#define ESC_RECALIBRATE_DAYS  30 /* don't recalibrate more than once per ~30 days     */
#define ESC_MAX_ATTEMPTS       4 /* give up (until next charge) after this many tries */

typedef struct EshifterCalibration {
    void *ipc;                   /* +0x08 IMQTTClient                    */
    void *clock;                 /* +0x10 steady-clock provider          */
    int   state;                 /* +0x18 enum esc_state                 */
    int   last_calibrated_day;   /* +0x1c epoch-day of last completion   */
    int   attempts;              /* +0x30 attempts since last completion */
} EshifterCalibration;

/* forward decls (the ctor wires these as subscription callbacks) */
void eshifter_on_last_calibrated(EshifterCalibration *self, const void *topic,
                                 const void *meta, const void *msg);
void eshifter_on_state_update(EshifterCalibration *self, const void *topic,
                              const void *meta, const void *msg);

static long epoch_day(EshifterCalibration *self)
{
    return (long)(clock_now_ns(self->clock) / 86400000000000ULL);
}

/* ------------------------------------------------------------------ *
 *  eshifter_calibration_ctor  (OEM 0x120030)
 * ------------------------------------------------------------------ */
void eshifter_calibration_ctor(EshifterCalibration *self, void *ipc, void *clock)
{
    self->ipc   = ipc;
    self->clock = clock;
    self->state = ESC_IDLE;
    self->last_calibrated_day = 0;
    self->attempts = 0;

    /* Restore the retained "last calibrated" day, then listen for gear updates. */
    esc_subscribe(ipc, "eshifter/last_calibrated",
                  (esc_cb)eshifter_on_last_calibrated, self);
    esc_subscribe(ipc, "eshifter/state",
                  (esc_cb)eshifter_on_state_update, self);
}

/* ------------------------------------------------------------------ *
 *  eshifter_on_last_calibrated  (OEM 0x120a40)
 *  The retained day-stamp we published when calibration last completed.
 * ------------------------------------------------------------------ */
void eshifter_on_last_calibrated(EshifterCalibration *self, const void *topic,
                                 const void *meta, const void *msg)
{
    (void)topic; (void)meta;
    if (json_is_number(msg))
        self->last_calibrated_day = (int)json_as_int(msg);
}

/* ------------------------------------------------------------------ *
 *  eshifter_request_calibration  (OEM 0x120680; eshifter_calibration.cpp:0x68)
 *  Called from battery_power_on_sequence on entry to CHARGING.
 * ------------------------------------------------------------------ */
void eshifter_request_calibration(EshifterCalibration *self)
{
    long day = epoch_day(self);
    long age = day - self->last_calibrated_day;

    /* Rate-limit, and never interrupt a calibration already in flight. */
    if (age < ESC_RECALIBRATE_DAYS || self->state != ESC_IDLE)
        return;

    common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x68, LOG_INFO,
                "Eshifter was last calibrated %d days ago, allowing calibration",
                (int)age);

    self->state = ESC_REQUEST;   /* the FSM sends it on the next eshifter/state update */
    self->attempts++;
}

/* ------------------------------------------------------------------ *
 *  eshifter_on_state_update  (OEM 0x120ac0; eshifter_calibration.cpp:0x30-0x46)
 *  Driven by "eshifter/state"; `current_gear` walks the calibration FSM.
 * ------------------------------------------------------------------ */
void eshifter_on_state_update(EshifterCalibration *self, const void *topic,
                              const void *meta, const void *msg)
{
    (void)topic; (void)meta;
    int gear = json_get_int(msg, "current_gear");

    switch (self->state) {
    case ESC_REQUEST:
        common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x30, LOG_INFO,
                    "Sending eshifter calibration request, current gear: %d", gear);
        ipc_publish_calibrate(self->ipc, "eshifter/gear/set");
        self->state = ESC_SENT;
        break;

    case ESC_SENT:
        common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x37, LOG_INFO,
                    "Eshifter calibration request sent, current gear: %d", gear);
        if (gear == 0)                      /* hub has dropped to gear 0: calibrating */
            self->state = ESC_IN_PROGRESS;
        break;

    case ESC_IN_PROGRESS:
        if (gear == 1) {                    /* back to 1st gear: finished */
            common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x3f,
                        LOG_INFO,
                        "Eshifter calibration has completed, eshifter is back to "
                        "1-st gear");
            self->last_calibrated_day = (int)epoch_day(self);
            self->attempts = 0;
            ipc_publish_day(self->ipc, "eshifter/last_calibrated",
                            self->last_calibrated_day);   /* retained */
            self->state = ESC_IDLE;
        } else {
            common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x46,
                        LOG_INFO,
                        "Eshifter calibration is in progress, current gear: %d", gear);
        }
        break;

    default:
        break;                              /* ESC_IDLE: nothing to do */
    }
}

/* ------------------------------------------------------------------ *
 *  eshifter_calibration_timeout  (OEM 0x11fee0; eshifter_calibration.cpp:0x55-0x5a)
 *  Watchdog: if no eshifter/state ever moved us out of a pending calibration,
 *  retry a few times, then block until the next charge.
 * ------------------------------------------------------------------ */
void eshifter_calibration_timeout(EshifterCalibration *self)
{
    if (self->state == ESC_IDLE)
        return;

    common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x55, LOG_INFO,
                "Eshifter state update not received, eshifter might be disabled");

    if (self->attempts < ESC_MAX_ATTEMPTS) {
        common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x57, LOG_INFO,
                    "Resetting calibration state to idle to try calibrating again");
        self->state = ESC_IDLE;
    } else {
        common_logf("devices/main/power/src/eshifter_calibration.cpp", 0x5a, LOG_ERR,
                    "Preventing further calibration attempts until next charging");
        od_override_block_calibration(true);
    }
}

/* ------------------------------------------------------------------ *
 *  eshifter_calibration_dtor  (OEM 0x120310)
 *  Unsubscribe and tear the request queue down. The OEM body is pure
 *  STL/std::variant teardown (vendor) — modelled here as the unsubscribes.
 * ------------------------------------------------------------------ */
void eshifter_calibration_dtor(EshifterCalibration *self)
{
    esc_unsubscribe(self->ipc, "eshifter/state");
    esc_unsubscribe(self->ipc, "eshifter/last_calibrated");
}
