/*
 * ux_service.c — UXService core, the central UX orchestrator.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000),
 * devices/main/ux/src/ux_service.cpp. UXService is a StateClient-derived
 * service owning a 7-state strategy machine (shipping/standby/operational/
 * charging/updating/alarm/maintenance). The active IStateStrategy* lives at
 * this+0x1088, the live state enum at this+0x1090, and the state-machine
 * context sub-object (handed to every strategy) at this+0x60.
 *
 * Vendor framework (StateClient, StorageManager, std::function managers,
 * nlohmann::json, the MQTT bus) is modelled as opaque handles + externs, not
 * reconstructed. OEM addresses are quoted per function.
 */
#include "ux_common.h"
#include "ux_service.h"

void ux_service_on_power_msg(ux_service *svc, const char *topic, const json_t *payload);

/* Field offsets inside the UXService object (relative to the service base). */
#define UX_OFF_STATE_CTX      0x60     /* state-machine context sub-object */
#define UX_OFF_ACTIVE_STRAT   0x1088   /* IStateStrategy* (current strategy)  */
#define UX_OFF_STATE_ENUM     0x1090   /* ux_state of the active strategy     */

/* The opaque service: we only ever touch it through the offsets above, so the
 * reconstruction works on a byte view obtained from the handle. */
static inline unsigned char *ux_bytes(ux_service *svc) { return (unsigned char *)svc; }

static inline ux_state_ctx *ux_ctx(ux_service *svc)
{
    return (ux_state_ctx *)(ux_bytes(svc) + UX_OFF_STATE_CTX);
}
static inline ux_strategy *ux_active_strategy(ux_service *svc)
{
    return *(ux_strategy **)(ux_bytes(svc) + UX_OFF_ACTIVE_STRAT);
}
static inline void ux_set_active_strategy(ux_service *svc, ux_strategy *s)
{
    *(ux_strategy **)(ux_bytes(svc) + UX_OFF_ACTIVE_STRAT) = s;
}
static inline int ux_prev_state(ux_service *svc)
{
    return *(int *)(ux_bytes(svc) + UX_OFF_STATE_ENUM);
}
static inline void ux_store_state(ux_service *svc, int st)
{
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_ENUM) = st;
}

/* ------------------------------------------------------------------------- *
 * UXService::UXService — 0x12e3e0
 *
 * Installs vtable_UXService at +0x0, embeds + initialises the StorageManager
 * (at this+0x60) wiring its publish/subscribe std::function callbacks, zeroes
 * the strategy machine (active=NULL @+0x1088, state=0 @+0x1090), constructs the
 * inner state machine, then subscribes two MQTT topics on the bus (param_2):
 *   "ux/power"        -> ux_service_on_power_msg
 *   "power/deep_sleep" (string @0x1af0b8 "ep_sleep"…) -> FUN_0012db60 handler.
 * ------------------------------------------------------------------------- */
void ux_service_ctor(ux_service *svc, void *bus, void *cfg_a, void *cfg_b, void *cfg_c)
{
    /* chain to the StateClient base ctor (FUN_00185a80) */
    state_client_base_ctor(svc);

    /* install our vtable */
    *(const void **)svc = (const void *)0 /* &vtable_UXService */;

    /* embed + init the StorageManager at this+0x60, passing the two std::function
     * callback closures (publish via _cb_manager_a, subscribe via _cb_manager_b).
     * The closures are torn down (op==3) after install. */
    storage_manager_init((void *)(ux_bytes(svc) + UX_OFF_STATE_CTX), bus,
                         cfg_b, cfg_c,
                         (ux_fn)ux_service_storage_cb_manager_a,
                         (ux_fn)ux_service_storage_cb_manager_b,
                         cfg_a);
    ux_service_storage_cb_manager_b(NULL, NULL, 3);
    ux_service_storage_cb_manager_a(NULL, NULL, 3);

    /* zero the strategy machine */
    ux_set_active_strategy(svc, NULL);
    ux_store_state(svc, 0);

    /* construct the inner state machine over (bus, state-ctx@+0x60) */
    ux_state_machine_init((void *)(ux_bytes(svc) + 0x1098), bus, ux_ctx(svc));

    /* subscribe "ux/power" (len 8, literal "xu/power" little-endian) */
    mqtt_subscribe((mqtt_client *)bus, "ux/power",
                   (mqtt_handler)ux_service_on_power_msg, svc);

    /* subscribe "power/deep_sleep" (string @0x1af0b8) -> deep-sleep handler */
    mqtt_subscribe((mqtt_client *)bus, "power/deep_sleep",
                   (mqtt_handler)0 /* FUN_0012db60 */, svc);
}

/* ------------------------------------------------------------------------- *
 * UXService::~UXService — 0x12e7a0
 *
 * Restores vtable_UXService, destroys the inner state machine (this+0x1098),
 * destroys the active IStateStrategy (this+0x1088 via [[vtbl]+8]), destroys the
 * sub-system clients + embedded StorageManager (this+0x60), and chains to the
 * StateClient base dtor.
 * ------------------------------------------------------------------------- */
void ux_service_dtor(ux_service *svc)
{
    ux_strategy *strat;

    *(const void **)svc = (const void *)0 /* &vtable_UXService */;

    ux_state_machine_dtor((void *)(ux_bytes(svc) + 0x1098));

    strat = ux_active_strategy(svc);
    if (strat != NULL)
        ux_strategy_destroy(strat);

    ux_subclients_dtor((void *)(ux_bytes(svc) + UX_OFF_STATE_CTX));
    state_client_base_dtor(svc);
}

/* ------------------------------------------------------------------------- *
 * UXService::run -> StateClient::start — 0x1854b0
 *
 * common/src/state_client.cpp:37 "Start." Subscribes the lifecycle/state MQTT
 * topics on the bus at this+0x38, then marks the client started (this+0x58=1).
 * The three topic strings are 0xb / 0x12 / 0x1a bytes built inline. Called by
 * main() after the ctor.
 * ------------------------------------------------------------------------- */
void ux_service_run(ux_service *svc)
{
    mqtt_client *bus = *(mqtt_client **)(ux_bytes(svc) + 0x38);

    common_logf("devices/main/common/src/state_client.cpp", 0x25, LOG_WARN, "Start.");

    /* topic 1 (len 0xb) */
    mqtt_subscribe(bus, "power/state",  (mqtt_handler)0 /* FUN_00185210 */, svc);
    /* topic 2 (len 0x12) */
    mqtt_subscribe(bus, "power/state/status", (mqtt_handler)0 /* FUN_00185260 */, svc);
    /* topic 3 (len 0x1a) */
    mqtt_subscribe(bus, "power/state/extend_timeout", (mqtt_handler)0 /* FUN_001852b0 */, svc);

    *(unsigned char *)(ux_bytes(svc) + 0x58) = 1; /* started */
}

/* ------------------------------------------------------------------------- *
 * UXService on-enter hook — 0x13ef20
 *
 * Tail-called by ToShipping after the strategy swap. Sets the state-changed
 * flag (ctx+0xf60 = 1) and persists the live state enum through the embedded
 * StorageManager (storage_manager_set at ctx+0x178, key at ctx+0x140).
 * (param is the state-ctx sub-object = service+0x60.)
 * ------------------------------------------------------------------------- */
void ux_service_on_state_entered(ux_service *svc, int new_state)
{
    unsigned char *ctx = ux_bytes(svc) + UX_OFF_STATE_CTX;
    /* value record: tag byte 4 (= int), payload 1; see storage manager codec */
    struct { unsigned char tag; unsigned char _pad[7]; long val; } record;

    *(int *)(ctx + 0xf60) = 1; /* state changed */

    record.tag = 4;
    record._pad[0] = record._pad[1] = record._pad[2] = 0;
    record._pad[3] = record._pad[4] = record._pad[5] = record._pad[6] = 0;
    record.val = 1;
    (void)new_state;

    /* key lives at ctx+0x140; StorageManager at ctx+0x178 */
    storage_manager_set((void *)(ctx + 0x178), (void *)(ctx + 0x140), &record);
}

/* ------------------------------------------------------------------------- *
 * MQTT handler for "ux/power" — 0x12e2c0
 *
 * Deserializes the bool payload; on false -> ux_power_off, on true ->
 * ux_power_on(ctx,0,0) against the state context at this+0x60.
 * ------------------------------------------------------------------------- */
void ux_service_on_power_msg(ux_service *svc, const char *topic, const json_t *payload)
{
    char on = 0;
    ux_state_ctx *ctx;
    (void)topic;

    ux_deserialize_bool(payload, &on);

    ctx = ux_ctx(svc);
    if (on == 0)
        ux_power_off(ctx);
    else
        ux_power_on(ctx, 0, 0);
}

/* ------------------------------------------------------------------------- *
 * The 7 UXService::To<State> virtuals.
 *
 * Each: log "To<State>" (WARN, ux_service.cpp), tear down the current strategy,
 * operator-new the concrete strategy of the correct size, construct it with the
 * previous state enum + the state-ctx (svc+0x60), publish it as the active
 * strategy (tearing down any racing predecessor), store the new state enum at
 * +0x1090, then run the on-enter step.
 *
 * ToShipping tail-calls the full on-enter hook (persist+publish). The other six
 * use the light inline variant that only writes the changed-state field
 * ctx+0xf60 = <new state> (FUN_0013dc60..0013dcb0).
 * ------------------------------------------------------------------------- */
static ux_strategy *ux_swap_strategy(ux_service *svc, void *obj,
                                     ux_strategy *(*ctor)(void *, int, ux_state_ctx *))
{
    ux_strategy *old, *neu;
    int prev = ux_prev_state(svc);

    /* clear-before-construct (OEM zeroes +0x1088 first so a re-entrant
     * teardown can't double-free) */
    old = ux_active_strategy(svc);
    ux_set_active_strategy(svc, NULL);
    if (old != NULL)
        ux_strategy_destroy(old);

    neu = ctor(obj, prev, ux_ctx(svc));

    old = ux_active_strategy(svc);
    ux_set_active_strategy(svc, neu);
    if (old != NULL)
        ux_strategy_destroy(old);

    return neu;
}

/* UXService::ToShipping — 0x12dbb0 (ux_service.cpp:54) */
void ux_to_shipping(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x36, LOG_WARN, "ToShipping");
    obj = op_new(UX_SZ_SHIPPING);
    ux_swap_strategy(svc, obj, state_shipping_strategy_ctor);
    ux_store_state(svc, UX_SHIPPING);
    ux_service_on_state_entered(svc, UX_SHIPPING);
}

/* UXService::ToStandby — 0x12dc70 (ux_service.cpp:62) */
void ux_to_standby(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x3e, LOG_WARN, "ToStandby");
    obj = op_new(UX_SZ_STANDBY);
    ux_swap_strategy(svc, obj, state_standby_strategy_ctor);
    ux_store_state(svc, UX_STANDBY);
    /* FUN_0013dc60: ctx+0xf60 = 2 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_STANDBY;
}

/* UXService::ToOperational — 0x12dd30 */
void ux_to_operational(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x46, LOG_WARN, "ToOperational");
    obj = op_new(UX_SZ_OPERATIONAL);
    ux_swap_strategy(svc, obj, state_operational_strategy_ctor);
    ux_store_state(svc, UX_OPERATIONAL);
    /* FUN_0013dc70: ctx+0xf60 = 3 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_OPERATIONAL;
}

/* UXService::ToCharging — 0x12ddf0 */
void ux_to_charging(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x4e, LOG_WARN, "ToCharging");
    obj = op_new(UX_SZ_CHARGING);
    ux_swap_strategy(svc, obj, state_charging_strategy_ctor);
    ux_store_state(svc, UX_CHARGING);
    /* FUN_0013dc80: ctx+0xf60 = 4 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_CHARGING;
}

/* UXService::ToUpdating — 0x12deb0 */
void ux_to_updating(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x56, LOG_WARN, "ToUpdating");
    obj = op_new(UX_SZ_UPDATING);
    ux_swap_strategy(svc, obj, state_updating_strategy_ctor);
    ux_store_state(svc, UX_UPDATING);
    /* FUN_0013dc90: ctx+0xf60 = 5 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_UPDATING;
}

/* UXService::ToAlarm — 0x12df70 */
void ux_to_alarm(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x5e, LOG_WARN, "ToAlarm");
    obj = op_new(UX_SZ_ALARM);
    ux_swap_strategy(svc, obj, state_alarm_strategy_ctor);
    ux_store_state(svc, UX_ALARM);
    /* FUN_0013dca0: ctx+0xf60 = 6 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_ALARM;
}

/* UXService::ToMaintenance — 0x12e030 */
void ux_to_maintenance(ux_service *svc)
{
    void *obj;
    common_logf("devices/main/ux/src/ux_service.cpp", 0x66, LOG_WARN, "ToMaintenance");
    obj = op_new(UX_SZ_MAINTENANCE);
    ux_swap_strategy(svc, obj, state_maintenance_strategy_ctor);
    ux_store_state(svc, UX_MAINTENANCE);
    /* FUN_0013dcb0: ctx+0xf60 = 7 */
    *(int *)(ux_bytes(svc) + UX_OFF_STATE_CTX + 0xf60) = UX_MAINTENANCE;
}
