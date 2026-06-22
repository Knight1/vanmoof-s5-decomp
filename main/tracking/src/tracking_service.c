/*
 * devices/main/tracking/src/tracking_service.cpp  (reconstructed)
 *
 * VanMoof S5/A5 'tracking' service — anti-theft / location state machine.
 * ELF: /usr/bin/tracking (AArch64, C++). Image base 0x100000.
 * OEM addresses are quoted per-function.
 *
 * TrackingApp is a common::StateClient subclass. It owns a 3-state theft
 * machine (0=OFF, 1=AUTO, 2=THEFT), a command worker thread draining an
 * int deque, and four MQTT subscriptions:
 *     "tracking/state"     -> on_tracking_state  (restore retained boot state)
 *     "modem/info/device"  -> on_modem_info      (log only)
 *     "modem/sms"          -> on_sms             (parse -> push command)
 *     "ux/alarm/state"     -> on_alarm_state     (IMU trigger -> AUTO)
 *
 * The framework objects (common::IMQTTClient / common::StateClient /
 * nlohmann::json / std::deque / std::thread / mutex / condition_variable)
 * are VENDOR; they are modelled through the opaque handles and extern
 * prototypes in tracking_common.h. The OEM open-codes the std::string topic
 * construction, the nlohmann::json publish-value building, and the deque
 * destruction glue inline; that boilerplate is folded into the helper calls
 * so the behaviour — not the allocator/STL dance — is preserved.
 */

#include "tracking_common.h"

#define SRC "devices/main/tracking/src/tracking_service.cpp"

/* MQTT topic literals (verbatim from the OEM rodata). */
#define TOPIC_TRACKING_STATE   "tracking/state"
#define TOPIC_MODEM_INFO        "modem/info/device"
#define TOPIC_MODEM_SMS         "modem/sms"
#define TOPIC_ALARM_STATE       "ux/alarm/state"
#define TOPIC_SOUND_PLAY        "ux/sound/play"

/* forward decls for the bound subscription trampolines */
static void tracking_service_on_tracking_state(void *ctx, const char *topic,
                                               void *meta, const json_value *payload);
static void tracking_service_on_modem_info(void *ctx, const char *topic,
                                           void *meta, const json_value *payload);
static void tracking_service_on_sms(void *ctx, const char *topic,
                                    void *meta, const json_value *payload);
static void tracking_service_on_alarm_state(void *ctx, const char *topic,
                                            void *meta, const json_value *payload);

/* ================================================================== *
 *  tracking_state_to_string                        OEM 0x00123890
 *  0=OFF, 1=AUTO, 2=THEFT, anything else -> "INVALID".
 *  (OEM returns a std::string by value; we return the literal.)
 * ================================================================== */
const char *tracking_state_to_string(int state)
{
    switch (state) {
    case TRACKING_OFF:   return "OFF";
    case TRACKING_AUTO:  return "AUTO";
    case TRACKING_THEFT: return "THEFT";
    default:             return "INVALID";
    }
}

/* ================================================================== *
 *  tracking_service_app_ctor                        OEM 0x00112e10
 *
 *  Lays out the 0x160-byte TrackingApp (StateClient subclass), wires the
 *  four MQTT subscriptions, and spawns the command worker thread.
 *    +0x60 mqtt  +0x68 state  +0x70 command deque  +0x150 worker thread
 *  The retained-state restore ("tracking/state") is bound first so the boot
 *  state is reloaded before any trigger arrives.
 * ================================================================== */
void tracking_service_app_ctor(TrackingApp *self, mqtt_client *mqtt,
                               state_client *state, void *service_env)
{
    (void)service_env;

    /* StateClient base ctor (installs vtable PTR_FUN_001572b8, the command
     * deque at +0x70 and the json map). The OEM moves the StateClient out
     * of the service-env into self->state (+0x68) and clears the env slot. */
    state_client_base_ctor(self, mqtt, state);
    self->mqtt  = mqtt;     /* +0x60 publish IMQTTClient */
    self->state = state;    /* +0x68 StateClient         */

    /* command deque empty; worker not yet running. */
    self->cmd_head = 0;
    self->cmd_tail = 0;
    self->cmd_mutex = 0;
    self->cmd_cv    = 0;
    self->worker    = 0;
    self->worker_run = false;

    /* ----- subscription table (IMQTTClient::subscribe = vtable+0x10) ---- *
     * Each entry binds a std::function trampoline to this App.            */
    mqtt_subscribe(mqtt, TOPIC_TRACKING_STATE,
                   &tracking_service_on_tracking_state, self, 1);
    mqtt_subscribe(mqtt, TOPIC_MODEM_INFO,
                   &tracking_service_on_modem_info, self, 1);
    mqtt_subscribe(mqtt, TOPIC_MODEM_SMS,
                   &tracking_service_on_sms, self, 1);
    mqtt_subscribe(mqtt, TOPIC_ALARM_STATE,
                   &tracking_service_on_alarm_state, self, 1);

    /* ----- command worker (std::thread at +0x150) --------------------- */
    self->worker = worker_spawn(&tracking_service_command_thread, self);
}

/* ================================================================== *
 *  tracking_service_app_dtor                        OEM 0x00112bc0
 *
 *  Reverses app_ctor: restores the base vtable, unsubscribes the four
 *  topics via IMQTTClient::unsubscribe (vtable+0x18), clears the worker
 *  run flag (+0x158), notifies the cv (+0x120) and joins the worker
 *  (+0x150), then frees the command deque and the json map.
 * ================================================================== */
void tracking_service_app_dtor(TrackingApp *self)
{
    /* unsubscribe order matches the OEM: sms, info/device, alarm/state.
     * (tracking/state is dropped with the base teardown.) */
    mqtt_unsubscribe(self->mqtt, TOPIC_MODEM_SMS);
    mqtt_unsubscribe(self->mqtt, TOPIC_MODEM_INFO);
    mqtt_unsubscribe(self->mqtt, TOPIC_ALARM_STATE);

    /* stop the worker: flag, wake the cv, then join. */
    self->worker_run = false;
    cmd_cv_notify(self);
    if (self->worker) {
        worker_join(self->worker);
        self->worker = 0;
    }

    /* free the command deque storage + nlohmann::json map (vendor). */
    tracking_app_base_dtor(self);
}

/* ================================================================== *
 *  tracking_service_app_delete                      OEM 0x00112de0
 *  Deleting destructor: app_dtor(self) then operator delete(self, 0x160).
 * ================================================================== */
void tracking_service_app_delete(TrackingApp *self)
{
    tracking_service_app_dtor(self);
    op_delete(self, 0x160);
}

/* ================================================================== *
 *  tracking_service_set_state                        OEM 0x00113810
 *
 *  Central mutator for the theft machine. Reads the current state via
 *  StateClient::getState (+0x18); no-op if unchanged. Otherwise logs and
 *  calls StateClient::setState (+0x10). When publish!=0 it also publishes
 *  a retained "tracking/state" (qos 1, retain 1) and flushes the client.
 *  (The OEM builds an nlohmann::json int node + open-codes the deque
 *  destruction afterwards; that STL glue is folded into the publish call.)
 * ================================================================== */
void tracking_service_set_state(TrackingApp *self, int new_state, int publish)
{
    /* held under the StateClient lock in the OEM (pthread mutex). */
    cmd_mutex_lock(self);

    if (state_client_get_state(self->state) == new_state) {
        cmd_mutex_unlock(self);
        return;
    }

    common_logf(SRC, 0x7b, LOG_INFO, "To tracking state : %s",
                tracking_state_to_string(new_state));

    state_client_set_state(self->state, new_state);

    if (publish) {
        /* retained publish of the new state value to "tracking/state". */
        mqtt_publish_int(self->mqtt, TOPIC_TRACKING_STATE,
                         (long)new_state, 1, 1);
        mqtt_flush(self->mqtt);
    }

    cmd_mutex_unlock(self);
}

/* ================================================================== *
 *  tracking_service_on_tracking_state                OEM 0x00114130
 *
 *  "tracking/state" observer — restores the retained boot state. Reads the
 *  int from the payload, logs it, then set_state(value, publish=0) (do NOT
 *  re-publish what we just read) and re-arms the StateClient start path.
 * ================================================================== */
static void tracking_service_on_tracking_state(void *ctx, const char *topic,
                                               void *meta, const json_value *payload)
{
    TrackingApp *self = (TrackingApp *)ctx;
    int state = 0;
    (void)topic; (void)meta;

    json_get_int(payload, &state);
    common_logf(SRC, 0x1f, LOG_INFO,
                "Retrieved tracking state from mqtt_broker: %s",
                tracking_state_to_string(state));

    json_get_int(payload, &state);
    tracking_service_set_state(self, state, 0);

    /* StateClient publish-client start (vtable +0x18 on +0x60). */
    state_client_start(self->mqtt, meta);
}

/* ================================================================== *
 *  tracking_service_on_modem_info                    OEM 0x00114240
 *  "modem/info/device" — log the device info string; no state effect.
 * ================================================================== */
static void tracking_service_on_modem_info(void *ctx, const char *topic,
                                           void *meta, const json_value *payload)
{
    (void)ctx; (void)topic; (void)meta;
    common_logf(SRC, 0x28, LOG_INFO, "Modem info device: %s",
                json_get_string(payload));
}

/* ================================================================== *
 *  tracking_service_on_sms                           OEM 0x00114300
 *
 *  "modem/sms" — log the SMS, parse it to a command code via
 *  sms_message_parse, push the code onto the command deque (+0x70 under
 *  the +0xc0 mutex) and notify the worker cv (+0x120).
 *  (The OEM open-codes the std::deque<int> push_back + nlohmann::json
 *  payload teardown; modelled by cmd_queue_push.)
 * ================================================================== */
static void tracking_service_on_sms(void *ctx, const char *topic,
                                    void *meta, const json_value *payload)
{
    TrackingApp *self = (TrackingApp *)ctx;
    int code;
    (void)topic; (void)meta;

    common_logf(SRC, 0x2f, LOG_INFO, "New sms: %s", json_get_string(payload));

    code = sms_message_parse(payload);

    /* push the code; cmd_queue_push takes +0xc0 and notifies +0x120. */
    cmd_queue_push(self, code);
}

/* ================================================================== *
 *  tracking_service_on_alarm_state                   OEM 0x00113d20
 *
 *  "ux/alarm/state" (published by ux after the Movement IMU trigger). Fires
 *  only when the alarm value equals 3 (json int/uint == 3, or double == 3.0).
 *  If the current tracking state is not already THEFT(2), it arms AUTO(1)
 *  with publish=1 — this is the IMU theft-arm entry point.
 * ================================================================== */
static void tracking_service_on_alarm_state(void *ctx, const char *topic,
                                            void *meta, const json_value *payload)
{
    TrackingApp *self = (TrackingApp *)ctx;
    int    ival = 0;
    double dval = 0.0;
    bool   is_three = false;
    (void)topic; (void)meta;

    /* OEM discriminates on the json type tag: 5=int, 6=uint, 7=double. */
    if (json_get_int(payload, &ival))
        is_three = (ival == 3);
    else if (json_get_number(payload, &dval))
        is_three = (dval == 3.0);

    if (!is_three)
        return;

    if (state_client_get_state(self->state) == TRACKING_THEFT)
        return;

    tracking_service_set_state(self, TRACKING_AUTO, 1);
}

/* ================================================================== *
 *  tracking_service_command_thread                   OEM 0x00113e30
 *
 *  Worker thread draining the command deque (+0x70 under +0xc0, cv +0x120).
 *  Sets the run flag (+0x158) on entry and loops while it is set:
 *    - empty queue -> wait on the cv
 *    - pop a code (a defensive "empty" log guards the race), dispatch:
 *        code 1 -> set_state(OFF=0,   publish=1)
 *        code 2 -> play_alarm_sound   (publish "ux/sound/play")
 *        code 0 -> set_state(THEFT=2, publish=1)
 *        other  -> log "Unknown message"
 * ================================================================== */
void tracking_service_command_thread(TrackingApp *self)
{
    self->worker_run = true;

    while (self->worker_run) {
        int code;

        cmd_mutex_lock(self);
        if (self->cmd_head == self->cmd_tail) {
            cmd_mutex_unlock(self);
            /* block until a command is pushed or the run flag clears. */
            cmd_cv_wait(self);
            if (!self->worker_run)
                break;
            continue;
        }
        cmd_mutex_unlock(self);

        if (!cmd_queue_pop(self, &code)) {
            common_logf(SRC, 0x5b, LOG_WARN, "Popped message is empty.");
            continue;
        }

        if (code == 1) {
            tracking_service_set_state(self, TRACKING_OFF, 1);
        } else if (code == 2) {
            tracking_service_play_alarm_sound(self);
        } else if (code == 0) {
            tracking_service_set_state(self, TRACKING_THEFT, 1);
        } else {
            common_logf(SRC, 0x6a, LOG_WARN, "Unknown message");
        }
    }
}

/* ================================================================== *
 *  tracking_service_play_alarm_sound                 OEM 0x00113350
 *
 *  Publishes the "locate" sound command to "ux/sound/play" via
 *  IMQTTClient::publish (+0x60 vtable+0x20, qos 1, retain 0). The OEM
 *  builds an nlohmann::json string node ("locate") and tears it down after
 *  the publish; that STL glue is folded into the publish call.
 * ================================================================== */
void tracking_service_play_alarm_sound(TrackingApp *self)
{
    cmd_mutex_lock(self);

    /* publish {"<sound>":"locate"} to ux/sound/play (qos 1, not retained). */
    mqtt_publish_str(self->mqtt, TOPIC_SOUND_PLAY, "locate", 1, 0);

    cmd_mutex_unlock(self);
}