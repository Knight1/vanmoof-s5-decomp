/*
 * alarm.c — VanMoof S5 i.MX8 `ux` service: Alarm strategy.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   Alarm::SetState  0x145dd0
 *   Alarm::Alarm     0x146460  (ctor)
 *   Alarm::~Alarm    0x1467c0  (dtor)
 *
 * The alarm object owns an MQTT publisher sub-object (this+0x78) and a
 * subscriber hashmap (this+0x11 slot). The publisher's std::function/container
 * internals and the OD-event hashmap are vendor framework — modelled at the
 * call site only (see CLAUDE.md "Decomp scope"). The reconstructed VanMoof
 * logic is: store the escalation level, log it, and (optionally) publish it.
 */
#include "ux_common.h"
#include "alarm.h"

/* --- vendor helpers referenced at the call sites (not reconstructed) ------- */
extern void  alarm_publisher_init(void *publisher, ux_service *ctx);   /* FUN_001869f0 */
extern void  alarm_publisher_register(void *publisher, long key);      /* FUN_00186da0 + cb */
extern void  alarm_publisher_destroy(void *publisher);                 /* FUN_001868a0 */
extern void *alarm_submap_new(size_t n);                               /* FUN_0010d710(0x110) */
extern void  alarm_submap_insert(void *map, ux_service **slot);        /* hashmap insert */
extern void  alarm_submap_erase(void *map, ux_service **slot);         /* FUN_00146c30 */
extern void  alarm_event_register_key1000(alarm_strategy *self);       /* FUN_00145bc0 OD key 1000 */
extern mqtt_client *alarm_publisher_client(void *publisher);           /* (this+0x78) as IMQTTClient */

/*
 * Alarm::SetState(this, state, publish) — 0x145dd0.
 *
 * Stores `state` at this+0x80, logs it, and (when publish) emits the level on
 * MQTT topic "ux/alarm/state" (qos=1, retain=1) through the publisher, then
 * flushes. States 1/2/3 are alarm escalation levels.
 */
void alarm_set_state(alarm_strategy *self, uint8_t state, bool publish)
{
    self->state = state;
    common_logf("devices/main/ux/src/alarm.cpp", 0x29, LOG_WARN,
                "Alarm::SetState %d", (unsigned)state);

    if (!publish)
        return;

    common_logf("devices/main/ux/src/alarm.cpp", 0x2c, LOG_WARN,
                "Alarm::SetState publish");

    /* publisher this+0x78, vt+0x20 = publish(topic, value, qos, retain).
       The OEM builds a JSON int with key "rm_state"; modelled as publish_int. */
    mqtt_publish_int(alarm_publisher_client(self->publisher),
                     "ux/alarm/state", (long)state, 1, 1);

    /* publisher vt+0x40 = flush */
    mqtt_flush(alarm_publisher_client(self->publisher));
}

/*
 * Alarm::Alarm(this, ctx) — 0x146460.
 *
 * Installs the vtable (DAT_001fa240), zero-inits the bookkeeping fields, sets
 * the default scale 0x3f800000 (=1.0f), builds the MQTT publisher (this+0x12
 * slot), registers the OD/event callback under key 1000, and allocates the
 * 0x110-byte subscriber hashmap (this+0x11 slot), inserting this object into it.
 */
void alarm_ctor(alarm_strategy *self, ux_service *ctx)
{
    /* zero the bookkeeping fields explicitly (no struct-init memset). */
    for (int i = 0; i < 14; i++)
        self->_containers[i] = 0;
    self->publisher       = 0;
    self->state           = 0;
    self->ctx             = ctx;
    self->subscriber_map  = 0;
    self->vtable          = (void *)0; /* OEM: &DAT_001fa240 */

    /* default scale 0x3f800000 == 1.0f lives in a container slot in the OEM;
       represented as part of the opaque container state. */

    alarm_publisher_init(&self->publisher, ctx);   /* FUN_001869f0 */
    alarm_event_register_key1000(self);            /* FUN_00186da0 + FUN_00145bc0, key 1000 */

    self->subscriber_map = alarm_submap_new(0x110); /* FUN_0010d710 */
    alarm_submap_insert(self->subscriber_map, &self->ctx);
}

/*
 * Alarm::~Alarm(this) — 0x1467c0.
 *
 * Restores the base vtable, removes this object from the subscriber hashmap
 * (this+0x11), tears down the MQTT publisher (this+0x12), and releases the map.
 */
void alarm_dtor(alarm_strategy *self)
{
    self->vtable = (void *)0; /* OEM: &DAT_001fa240 (restore base) */

    if (self->subscriber_map != 0) {
        alarm_submap_erase(self->subscriber_map, &self->ctx); /* FUN_00146c30 */
        self->subscriber_map = 0;
    }

    alarm_publisher_destroy(&self->publisher); /* FUN_001868a0 */
}
