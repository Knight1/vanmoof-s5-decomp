#include "tracking_common.h"

/*
 * movement.c — VanMoof i.MX8 "tracking" service: Movement / IMU detection
 *
 * The Movement object models the bike's coarse motion state (kMoving /
 * Stationary) inferred from IMU alarm triggers delivered over MQTT. It is
 * constructed in tracking_main and then handed to cell_locator_ctor, so the
 * cellular poller can read movement state and choose a poll cadence.
 *
 * Behaviour (verbatim from the decompiled OEM, base 0x100000):
 *   - On construction it subscribes the MQTT topic
 *       "ux/tracking/alarm/imu/triggered"   (string @0x0013a318)
 *     binding movement_set_moving as the message handler. It then arms a
 *     periodic timer with interval 1800000 ms (30 minutes) whose expiry
 *     callback is movement_set_stationary.
 *   - Each IMU trigger marks the state kMoving (debounced) and (re)starts the
 *     30-minute window; when no further trigger arrives within that window the
 *     timer fires and reverts the state to Stationary.
 *   - State transitions walk a registered listener list (std::deque of
 *     void(int) callbacks) under a mutex, invoking each with the new state
 *     value (1 = kMoving, 2 = Stationary).
 *
 * Source: devices/main/tracking/src/movement.cpp
 *
 * OEM object layout (see Movement struct in tracking_common.h):
 *   +0x08  mutex                 guards the listener list
 *   +0x38  listener list         std::deque<std::function<void(int)>>
 *   +0xa0  MovementState         0=unset, 1=kMoving, 2=Stationary
 *   +0x0e  IMQTTClient*          transport (ctor arg)
 *   +0x0f  periodic_timer*       30-min revert-to-Stationary timer
 *
 * The std::function / std::deque / timer / mutex framework is MODELLED here
 * (opaque typedefs + externs); the *_fn_manager / *_listener_* STL glue is
 * compiler-emitted and is NOT reconstructed. This TU compiles standalone for
 * behavioural review.
 */

/* Movement vtable @0x00156be8 (dtor / deleting-dtor / state accessors). */
extern const void *const PTR_movement_dtor_00156be8;

/* ------------------------------------------------------------------ */
/* state transitions                                                  */
/* ------------------------------------------------------------------ */
/*
 * In the OEM, setState walks the std::deque<std::function<void(int)>>
 * listener list under the lock and invokes each with the new state, and each
 * per-listener trampoline (@0x111d00 / @0x111d50) emits the state log line.
 * The deque/std::function iteration is vendor STL glue; it is modelled here as
 * a single movement_listeners_notify() broadcast, and the log line (emitted
 * per-listener in the OEM) is reproduced once on the transition.
 */
#define MOVEMENT_SRC "devices/main/tracking/src/movement.cpp"

/*
 * movement_set_moving — Movement::setState(kMoving) — @0x001121e0
 *
 * Bound as the IMU-trigger MQTT handler in movement_ctor. No-op if already
 * kMoving (debounce: repeated triggers don't re-broadcast). Otherwise it
 * broadcasts kMoving to the listeners under the lock and records the state.
 */
void movement_set_moving(Movement *self) /* @0x001121e0 */
{
    /* OEM locks an inner timer/mutex object first; debounce reads the state. */
    if (self->state == MOVEMENT_STATE_MOVING)
        return;

    common_logf(MOVEMENT_SRC, 0x23, LOG_WARN, "MovementState : kMoving"); /* movement.cpp:35 */

    movement_mutex_lock(&self->lock);
    movement_listeners_notify(self->listeners, MOVEMENT_STATE_MOVING);
    movement_mutex_unlock(&self->lock);

    self->state = MOVEMENT_STATE_MOVING;
}

/*
 * movement_set_stationary — Movement::setState(Stationary) — @0x00112060
 *
 * Bound as the 30-min periodic-timer expiry callback in movement_ctor.
 * Broadcasts Stationary to the listeners and records the state. (Unlike
 * set_moving there is no early-out debounce in the OEM.) The OEM log string
 * carries the typo "Sationary" — reproduced verbatim.
 */
void movement_set_stationary(Movement *self) /* @0x00112060 */
{
    common_logf(MOVEMENT_SRC, 0x2f, LOG_WARN, "MovementState : Sationary"); /* movement.cpp:47 [sic] */

    movement_mutex_lock(&self->lock);
    movement_listeners_notify(self->listeners, MOVEMENT_STATE_STATIONARY);
    movement_mutex_unlock(&self->lock);
    self->state = MOVEMENT_STATE_STATIONARY;
}

/* ------------------------------------------------------------------ */
/* IMU-trigger MQTT handler                                           */
/* ------------------------------------------------------------------ */

/*
 * IMU-trigger subscribe lambda — installed by movement_ctor.
 *
 * Movement::Movement(IMQTTClient&)::{lambda(int, const std::string&,
 * const nlohmann::json&)} (demangle hint @0x0013a338). Every published
 * "ux/tracking/alarm/imu/triggered" message just marks the object kMoving;
 * the JSON payload is not inspected.
 */
static void movement_on_imu_triggered(void *ctx, void *a,
                                      const char *topic,
                                      const mqtt_msg *msg) /* lambda @ ctor */
{
    (void)a;
    (void)topic;
    (void)msg;
    movement_set_moving((Movement *)ctx);
}

/* ------------------------------------------------------------------ */
/* ctor / dtor                                                        */
/* ------------------------------------------------------------------ */

/*
 * Movement::Movement(common::IMQTTClient&) — @0x00111df0
 *
 * Constructs the 216-byte Movement object (vtable @0x00156be8):
 *   - zeroes the listener list and mutex,
 *   - stores the IMQTTClient reference,
 *   - constructs the 0xb0-byte periodic timer bound to the client,
 *   - subscribes "ux/tracking/alarm/imu/triggered" (QoS 1) with
 *     movement_on_imu_triggered, registering movement_set_moving as the
 *     first state listener (state := kMoving),
 *   - arms the timer with interval 1800000 ms (30 min) and expiry callback
 *     movement_set_stationary (state := Stationary).
 */
void movement_ctor(Movement *self, IMqttClient *mqtt) /* @0x00111df0 */
{
    self->vtable = &PTR_movement_dtor_00156be8;

    /* zero the listener list + state (no struct-init memset). */
    self->listeners = NULL;
    self->state = MOVEMENT_STATE_UNSET;

    self->mqtt = mqtt;

    /* 0xb0-byte periodic timer, bound to the event loop / MQTT client. */
    self->revert_timer = timer_new(mqtt);             /* FUN_001074e0 + FUN_00122a60 */

    /* subscribe the IMU-trigger topic (vtable+0x10, QoS 1); the first state
     * listener bound is movement_set_moving (state -> kMoving on trigger). */
    mqtt_subscribe(mqtt, MOVEMENT_IMU_TOPIC,
                   (mqtt_handler)movement_on_imu_triggered, self, 1);

    /* arm the 30-minute no-motion revert timer: on expiry the object goes
     * back to Stationary. */
    timer_arm(self->revert_timer,
              (timer_cb_t)movement_set_stationary, self, 1800000); /* FUN_00122e10 */
}

/*
 * Movement::~Movement() — @0x00112380
 *
 * Stops/destroys the periodic timer, unsubscribes the IMU-trigger topic
 * (vtable+0x18), releases the stationary std::function, and frees the
 * listener list. Called from tracking_main on shutdown.
 */
void movement_dtor(Movement *self) /* @0x00112380 */
{
    self->vtable = &PTR_movement_dtor_00156be8;

    /* stop the timer first so no expiry can race the teardown. */
    timer_stop_free(self->revert_timer);              /* FUN_001227d0 */

    mqtt_unsubscribe(self->mqtt, MOVEMENT_IMU_TOPIC);

    movement_listener_list_free(self->listeners);
    self->revert_timer = NULL;
    self->listeners = NULL;
}