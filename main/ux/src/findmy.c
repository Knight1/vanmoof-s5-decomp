/*
 * findmy.c — VanMoof S5 i.MX8 `ux` service: Apple Find-My (FMNA) strategy.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   FindMy::FindMy            0x13b820  (ctor + MQTT subscription glue)
 *   FindMy::Toggle            0x13a7e0
 *   FindMy::PublishCertified  0x139d00
 *
 * The ctor wires the UX side of the BLE Find-My pairing/control protocol:
 * it subscribes to a fixed set of ble/findmy topics (handlers
 * FUN_001380d0..FUN_001383e0 — vendor std::function glue), then reads the
 * storage "certified" flag to decide whether Find-My is allowed on this bike,
 * and finally publishes the certified flag.
 *
 * The nlohmann::json builders and the publisher/subscriber container internals
 * are vendor framework: modelled at the call site via ux_common's mqtt_* and
 * json_* helpers (see CLAUDE.md "Decomp scope").
 */
#include "ux_common.h"
#include "findmy.h"

/* --- vendor helpers referenced at the call sites (not reconstructed) ------- */
extern mqtt_client *findmy_publisher_client(void *publisher);   /* this[1] as IMQTTClient */
extern bool storage_manager_load_field_120(void *storage, void *out); /* certified flag */

/*
 * The ctor subscribes to these BLE Find-My topics, in OEM order. Each entry's
 * handler is a vendor std::function (FUN_001380d0..FUN_001383e0); the
 * reconstructed VanMoof logic is purely the subscription wiring, so the handler
 * is modelled as an opaque thunk per topic.
 */
static void findmy_on_sound          (void *c, const char *t, const json_t *p); /* FUN_001380d0 */
static void findmy_on_reset          (void *c, const char *t, const json_t *p); /* FUN_00138110 */
static void findmy_on_reset_ignored  (void *c, const char *t, const json_t *p); /* FUN_00138160 */
static void findmy_on_pairing        (void *c, const char *t, const json_t *p); /* FUN_001381b0 */
static void findmy_on_pairing_failed (void *c, const char *t, const json_t *p); /* FUN_00138200 */
static void findmy_on_paired         (void *c, const char *t, const json_t *p); /* FUN_00138250 */
static void findmy_on_enable         (void *c, const char *t, const json_t *p); /* FUN_001382a0 */
static void findmy_on_disable        (void *c, const char *t, const json_t *p); /* FUN_001382f0 */
static void findmy_on_provisioned    (void *c, const char *t, const json_t *p); /* FUN_00138340 */
static void findmy_on_report         (void *c, const char *t, const json_t *p); /* FUN_00138390 */
static void findmy_on_paired2        (void *c, const char *t, const json_t *p); /* FUN_001383e0 */

/*
 * FindMy::FindMy(this, publisher, a, storage, ctx) — 0x13b820.
 */
void findmy_ctor(findmy_strategy *self, void *publisher, void *a,
                 void *storage, ux_service *ctx)
{
    mqtt_client *c;

    self->vtable    = (void *)0; /* OEM: &DAT_001fa1b8 */
    self->publisher = publisher;
    self->a         = a;
    self->storage   = storage;
    self->ctx       = ctx;

    /* zero the subscription bookkeeping + state fields explicitly. */
    for (int i = 0; i < 6; i++)
        self->_slots[i] = 0;
    self->certified     = 0;
    self->enabled       = 0;
    self->_pad5a        = 0;
    self->paired        = 0;
    self->pairing_state = -1; /* 0xffffffff */

    /* register this object with the UXService subscriber hashmap (vendor). */

    c = findmy_publisher_client(self->publisher); /* this[1] vt+0x10 = subscribe */

    /* Fixed Find-My subscription set, OEM order (topic byte-lengths verbatim). */
    mqtt_subscribe(c, "ble/findmy/sound",                findmy_on_sound,          self);
    mqtt_subscribe(c, "ble/findmy/event/reset",          findmy_on_reset,          self);
    mqtt_subscribe(c, "ble/findmy/event/reset_ignored",  findmy_on_reset_ignored,  self);
    mqtt_subscribe(c, "ble/findmy/event/pairing",        findmy_on_pairing,        self);
    mqtt_subscribe(c, "ble/findmy/event/pairing_failed", findmy_on_pairing_failed, self);
    mqtt_subscribe(c, "ble/findmy/event/paired",         findmy_on_paired,         self);
    mqtt_subscribe(c, "ble/findmy/event/enable",         findmy_on_enable,         self);
    mqtt_subscribe(c, "ble/findmy/event/disable",        findmy_on_disable,        self);
    mqtt_subscribe(c, "ble/findmy/provisioned",          findmy_on_provisioned,    self);
    mqtt_subscribe(c, "ble/findmy/report",               findmy_on_report,         self);
    mqtt_subscribe(c, "ble/findmy/certify",               findmy_on_paired2,        self);

    /* storage "certified" flag: load returns 0 (false) when the field IS set. */
    if (!storage_manager_load_field_120(self->storage, &self->certified)) {
        common_logf("devices/main/ux/src/findmy.cpp", 0x47, LOG_WARN,
                    "FindMy certified bike");
        self->certified = 1;
    } else {
        common_logf("devices/main/ux/src/findmy.cpp", 0x45, LOG_WARN,
                    "This is a concierge bike, findmy not allowed");
    }

    findmy_publish_certified(self);
}

/*
 * FindMy::Toggle(this) — 0x13a7e0.
 *
 * Drives FMNA pairing/control from the UX side: if either `enabled` (+0x59) or
 * `paired` (+0x5c) is set, it builds JSON {"toggle": true} and publishes it to
 * "ble/findmy/control" (qos=1, retain=0) via the publisher (this[1] vt+0x20).
 */
void findmy_toggle(findmy_strategy *self)
{
    json_t body;
    mqtt_client *c;

    if (self->enabled == 0 && self->paired == 0)
        return;

    common_logf("devices/main/ux/src/findmy.cpp", 0x6a, LOG_WARN, "FindMy toggle");

    /* build {"toggle": true} (nlohmann::json; modelled). */
    body._opaque = 0;
    /* json["toggle"] = true; */

    c = findmy_publisher_client(self->publisher);
    mqtt_publish_json(c, "ble/findmy/control", &body, 1, 0);

    json_free(&body);
}

/*
 * FindMy::PublishCertified(this) — 0x139d00.
 *
 * Builds JSON {"certified": <this+0x58 bool>} and publishes it (retain=1) to
 * "ble/findmy/certified" via the publisher (this[1] vt+0x20). Called at the
 * end of the ctor.
 */
void findmy_publish_certified(findmy_strategy *self)
{
    json_t body;
    mqtt_client *c;

    /* build {"certified": (bool)self->certified}. */
    body._opaque = 0;
    /* json["certified"] = (this+0x58 != 0); */

    c = findmy_publisher_client(self->publisher);
    mqtt_publish_json(c, "ble/findmy/certified", &body, 1, 1);

    json_free(&body);
}

/* --- subscription handlers (vendor std::function glue, modelled) ----------- */
static void findmy_on_sound(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_001380d0 */ }
static void findmy_on_reset(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138110 */ }
static void findmy_on_reset_ignored(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138160 */ }
static void findmy_on_pairing(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_001381b0 */ }
static void findmy_on_pairing_failed(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138200 */ }
static void findmy_on_paired(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138250 */ }
static void findmy_on_enable(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_001382a0 */ }
static void findmy_on_disable(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_001382f0 */ }
static void findmy_on_provisioned(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138340 */ }
static void findmy_on_report(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_00138390 */ }
static void findmy_on_paired2(void *c, const char *t, const json_t *p)
{ (void)c; (void)t; (void)p; /* FUN_001383e0 */ }
