/*
 * cell_locator.c — CellLocator: cellular-tower location poller (reconstructed)
 *
 * OEM: /usr/bin/tracking, devices/main/tracking/src/cell_locator.cpp.
 *   CellLocator::CellLocator       0x10af70  (IMQTTClient&, Movement&, lambda)
 *   CellLocator::_on_location      0x10b410  (modem/location/cellular handler)
 *   CellLocator::_poll_thread      0x10b6d0  (fix-request poll loop)
 *
 * Behaviour-oriented translation (C++ modelled in C). The MQTT client, Movement,
 * nlohmann::json message, std::thread / mutex / condition_variable, and the
 * std::function callback wrapper are VENDOR framework — modelled as opaque
 * typedefs + extern prototypes (see tracking_common.h). The std::thread
 * trampoline (cell_locator_poll_thread_invoke) and the lambda-capture / SSO
 * std::string construction inlined into the ctor are STL glue; only the call
 * sites are reproduced here.
 *
 * CellLocator only *requests* cellular fixes on a poll interval and logs the
 * async reply — it computes no location estimate and re-publishes nothing.
 */
#include "tracking_common.h"

#define CL_FILE "devices/main/tracking/src/cell_locator.cpp"

/* Topic the modem service publishes cellular fixes on (str @ 0x13a130). */
#define TOPIC_LOCATION_CELLULAR "modem/location/cellular"

/* Default poll interval, minutes (ctor self+0x14 = 5). */
#define CL_DEFAULT_INTERVAL_MIN 5

/* Tracking-type -> poll-interval (minutes) map (poll_thread). The packed 64-bit
 * value at cl->tracking_type is set from "modem/tracking/type/set". */
#define TT_HIGH_FREQ   0x100000002L  /* ->   2 min */
#define TT_BALANCED    0x200000002L  /* ->  30 min */
#define TT_LOW_FREQ    0x200000001L  /* -> 240 min */
#define INTERVAL_HIGH_FREQ    2
#define INTERVAL_BALANCED    30
#define INTERVAL_LOW_FREQ   240
#define INTERVAL_DEFAULT      5

/* ms-per-minute used to size the condition_variable timed wait. */
#define MS_PER_MINUTE 60000

/*
 * OEM 0x10b410 — CellLocator::_on_location(self, .., json& payload).
 *
 * MQTT handler for "modem/location/cellular", registered in the ctor. Renders
 * the inbound cellular-location JSON to text (json::dump, FUN_00111730, indent
 * -1) and logs it. Pure observer in this build: the raw modem fix
 * (towers / MCC / MNC / LAC / CID) is produced by the modem service; CellLocator
 * just records the reply it provoked. No re-publish, no estimate.
 */
void cell_locator_on_location(void *ctx, const char *topic, void *meta,
                              const json_t *payload)
{
    char *text;

    (void)ctx;
    (void)topic;
    (void)meta;

    /* json::dump(payload, indent=-1, ' ', ...) -> heap std::string */
    text = json_dump(payload, -1);

    /* cell_locator.cpp:39, level 3 (WARN) */
    common_logf(CL_FILE, 0x27, LOG_WARN,
                "Modem - cellular location : %s", text);

    json_dump_free(text);
}

/*
 * OEM 0x10b6d0 — CellLocator::_poll_thread(self).
 *
 * Spawned by the ctor. Runs under cl->mutex (run flag cl->running set on entry).
 * Each cycle:
 *   - If no tracking type is selected yet (tracking_type == 0), just notify the
 *     condition_variable and re-loop (stays parked until a type arrives).
 *   - Otherwise map the packed tracking_type to a poll interval (minutes),
 *     notify the cv, request a cellular fix from the modem through the MQTT
 *     client vtable (+0xc8: request_cellular_fix), then do a timed cv wait of
 *     interval*60000 ms, re-checking the stop flag.
 * The fix reply arrives asynchronously on "modem/location/cellular" and is
 * handled by cell_locator_on_location.
 */
void cell_locator_poll_thread(CellLocator *self)
{
    long interval_min;

    mutex_lock(&self->mutex);
    self->running = 1;

    for (;;) {
        if (!self->running) {
            mutex_unlock(&self->mutex);
            return;
        }

        if (self->tracking_type == 0) {
            /* no tracking type yet — park on the cv and re-evaluate */
            cond_notify(&self->cond);
            cond_wait(&self->cond, &self->mutex);
            continue;
        }

        /* map packed tracking-type token to interval in minutes */
        switch (self->tracking_type) {
        case TT_HIGH_FREQ:
            interval_min = INTERVAL_HIGH_FREQ;
            break;
        case TT_BALANCED:
            interval_min = INTERVAL_BALANCED;
            break;
        case TT_LOW_FREQ:
            interval_min = INTERVAL_LOW_FREQ;
            break;
        default:
            interval_min = INTERVAL_DEFAULT;
            break;
        }
        self->poll_interval_min = interval_min;

        cond_notify(&self->cond);

        if (self->client == NULL) {
            /* OEM logs/aborts on a null client (FUN_001070b0) */
            mqtt_null_client_abort();
            return;
        }

        /* request a cellular fix: client vtable +0xc8 */
        mqtt_request_cellular_fix(self->client);

        /* timed wait: interval*60000 ms, woken early by stop/notify */
        cond_wait_for(&self->cond, &self->mutex,
                      self->poll_interval_min * MS_PER_MINUTE);
    }
}

/*
 * OEM 0x10af70 — CellLocator::CellLocator(this, IMQTTClient& mqtt,
 *                                          Movement& mvmt, lambda).
 *
 * RTTI class name "CellLocator" (typeinfo @ 0x13a151). Wires up the poller:
 *   - stores the MQTT client and the Movement reference,
 *   - default poll interval = 5 minutes,
 *   - constructs the thread state object (mutex + condition_variable, 0xb0 bytes),
 *   - subscribes "modem/location/cellular" -> _on_location with QoS 1,
 *   - spawns the _poll_thread std::thread.
 * The 3rd ctor arg (the movement lambda) is stored in the std::function slot for
 * Movement notifications; its capture-bind plumbing is STL glue and is modelled
 * here as a single assignment.
 */
void cell_locator_ctor(CellLocator *self, IMQTTClient *mqtt, Movement *mvmt,
                       cl_movement_fn movement_cb)
{
    /* zero the POD/flag fields explicitly (no struct-init memset) */
    self->client = mqtt;                 /* self+0x18 */
    self->movement = mvmt;               /* self+0x20 */
    self->running = 0;
    self->stop_requested = 0;
    self->tracking_type = 0;
    self->poll_interval_min = 0;
    self->thread = 0;
    self->movement_cb = movement_cb;     /* self+0x16 std::function slot */

    /* default poll interval (self+0x14) */
    self->poll_interval_min = CL_DEFAULT_INTERVAL_MIN;

    /* thread state object: mutex + condition_variable (0xb0 bytes, FUN_00122a60) */
    thread_state_init(&self->cond, &self->mutex);

    /* subscribe to the modem's cellular-fix replies (QoS 1) */
    mqtt_subscribe(self->client, TOPIC_LOCATION_CELLULAR,
                   cell_locator_on_location, self, 1);

    /* spawn the poll thread (trampoline cell_locator_poll_thread_invoke) */
    self->thread = thread_spawn((thread_entry_t)cell_locator_poll_thread, self);
}
