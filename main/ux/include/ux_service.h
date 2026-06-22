/*
 * ux_service.h — UXService core: the central UX orchestrator object.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000),
 * source path devices/main/ux/src/ux_service.cpp. UXService derives from the
 * common StateClient<...> framework base and owns the 6+1 state strategy
 * machine. These decls cover the layout the core TU touches; the vendor
 * StateClient/StorageManager internals are modelled opaquely.
 */
#ifndef UX_SERVICE_H
#define UX_SERVICE_H

#include "ux_common.h"

/*
 * The strategy interface every state object presents. Only the virtual
 * destructor (vtable slot +8, the [[vtbl]+8] called on swap) is used by the
 * core; the rest is opaque.
 */
typedef struct ux_strategy ux_strategy;

/*
 * The state-machine context sub-object that lives at offset +0x60 of a
 * UXService and is handed to every strategy ctor and to the on-enter hook.
 * In the OEM image it is the StorageManager-bearing StateClient context;
 * here we expose only the two fields the core TU writes.
 */
typedef struct ux_state_ctx ux_state_ctx;

/*
 * Operator-new sizes of each concrete strategy (OEM allocation sizes), so the
 * To<State> swaps allocate the right object. Verified from the new() calls.
 */
#define UX_SZ_SHIPPING     0x10u
#define UX_SZ_STANDBY      0x5a0u
#define UX_SZ_OPERATIONAL  0x440u
#define UX_SZ_CHARGING     0xe8u
#define UX_SZ_UPDATING     0x18u
#define UX_SZ_ALARM        0x5c0u
#define UX_SZ_MAINTENANCE  0x98u

/* Concrete strategy constructors (placement: ctor(obj, prev_state, state_ctx)). */
ux_strategy *state_shipping_strategy_ctor   (void *obj, int prev_state, ux_state_ctx *ctx); /* 0x134d60 */
ux_strategy *state_standby_strategy_ctor    (void *obj, int prev_state, ux_state_ctx *ctx); /* 0x133d50 */
ux_strategy *state_operational_strategy_ctor(void *obj, int prev_state, ux_state_ctx *ctx); /* 0x131900 */
ux_strategy *state_charging_strategy_ctor   (void *obj, int prev_state, ux_state_ctx *ctx); /* 0x12ef30 */
ux_strategy *state_updating_strategy_ctor   (void *obj, int prev_state, ux_state_ctx *ctx); /* 0x134ea0 */
ux_strategy *state_alarm_strategy_ctor      (void *obj, int prev_state, ux_state_ctx *ctx); /* 0x135e60 */
ux_strategy *state_maintenance_strategy_ctor(void *obj, int prev_state, ux_state_ctx *ctx); /* 0x136a10 */

/* Strategy virtual-destructor invoker (vtable slot +8) used on every swap. */
void ux_strategy_destroy(ux_strategy *s);

/*
 * StorageManager embedded at this+0x60+0x118 (== this+0x178). The core TU
 * persists the live state enum through it. Modelled opaquely; the field key
 * lives at state_ctx+0x140 (this+0x1a0).
 */
void storage_manager_init(void *sm, void *bus, void *a, void *b, ux_fn cb_a, ux_fn cb_b, void *cfg); /* 0x141ea0 */
void storage_manager_set (void *sm, const void *key, const void *value);                            /* 0x119da0 */

/*
 * vendor std::function manager thunks for the embedded StorageManager publish/
 * subscribe callbacks (see export: ux_service_storage_cb_manager_a/_b). The
 * core only needs to model that they are installed + torn down at ctor end.
 */
void ux_service_storage_cb_manager_a(void *a, void *b, int op); /* 0x12e910 */
void ux_service_storage_cb_manager_b(void *a, void *b, int op); /* 0x12e9c0 */

/* StateClient base ctor/dtor + the per-strategy machine init helper. */
void state_client_base_ctor(ux_service *svc);                 /* FUN_00185a80 */
void state_client_base_dtor(ux_service *svc);                 /* FUN_001857f0 */
void ux_state_machine_init (void *machine, void *bus, ux_state_ctx *ctx); /* FUN_00137440 */
void ux_state_machine_dtor (void *machine);                  /* FUN_00137a30 */
void ux_subclients_dtor    (void *clients);                  /* FUN_00143970 */

/* power-path entrypoints driven by the ux/power MQTT handler. */
void ux_power_off(ux_state_ctx *ctx);                /* FUN_0013e2a0 */
void ux_power_on (ux_state_ctx *ctx, int a, int b);  /* FUN_0013e1b0 */
bool ux_deserialize_bool(const json_t *payload, char *out); /* FUN_0012ea70 (model) */

#endif /* UX_SERVICE_H */
