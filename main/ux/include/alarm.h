#ifndef UX_ALARM_H
#define UX_ALARM_H

#include "ux_common.h"

/*
 * alarm.cpp — Alarm strategy object (program "ux", AArch64, base 0x100000).
 *
 * Layout reconstructed from the ctor/dtor at 0x146460 / 0x1467c0 and
 * Alarm::SetState at 0x145dd0. Only the fields the reconstructed VanMoof logic
 * touches are modelled; the std::function / unordered-map glue and the publisher
 * sub-object are opaque (VENDOR).
 */
typedef struct alarm_strategy {
    void   *vtable;            /* +0x00  DAT_001fa240 */
    /* +0x08 .. +0x70: subscriber/event bookkeeping (vendor containers) */
    void   *_containers[14];
    void   *publisher;         /* +0x78  MQTT publisher sub-object (vt+0x20 = publish) */
    uint8_t state;             /* +0x80  current alarm escalation level (1/2/3) */
    ux_service *ctx;           /* UXService context (passed to the ctor) */
    void   *subscriber_map;    /* +0x11 slot: OD/event subscriber hashmap (0x110 bytes) */
} alarm_strategy;

/* MQTT publisher vtable used by SetState (this+0x78): slot 0x20 = publish,
   slot 0x40 = flush. Modelled via ux_common's mqtt_* helpers. */

void alarm_set_state(alarm_strategy *self, uint8_t state, bool publish); /* 0x145dd0 */
void alarm_ctor(alarm_strategy *self, ux_service *ctx);                  /* 0x146460 */
void alarm_dtor(alarm_strategy *self);                                   /* 0x1467c0 */

#endif /* UX_ALARM_H */
