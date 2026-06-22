#ifndef UX_FINDMY_H
#define UX_FINDMY_H

#include "ux_common.h"

/*
 * findmy.cpp — FindMy (Apple Find-My / FMNA) strategy object.
 * program "ux", AArch64, base 0x100000.
 *
 * Reconstructed from FindMy::FindMy 0x13b820, FindMy::Toggle 0x13a7e0,
 * FindMy::PublishCertified 0x139d00. The publisher (this[1]) and storage
 * (this[3]) sub-objects, and the std::function subscription glue, are vendor.
 */
typedef struct findmy_strategy {
    void   *vtable;       /* [0]  +0x00  DAT_001fa1b8 */
    void   *publisher;    /* [1]  +0x08  MQTT publisher (vt+0x10 subscribe, vt+0x20 publish) */
    void   *a;            /* [2]  +0x10  ctor arg `a` */
    void   *storage;      /* [3]  +0x18  storage backend (certified flag) */
    void   *ctx;          /* [4]  +0x20  UXService context */
    void   *_slots[6];    /* [5..10] +0x28..+0x57 subscription bookkeeping (vendor) */
    uint8_t certified;    /* +0x58  FindMy certified bike */
    uint8_t enabled;      /* +0x59  pairing/control enabled */
    uint8_t _pad5a;       /* +0x5a */
    uint8_t paired;       /* +0x5c  paired/active (gates Toggle) */
    int32_t pairing_state;/* +0x60  -1 (0xffffffff) until pairing progresses */
} findmy_strategy;

/* subscription handlers FUN_001380d0..FUN_001383e0 (vendor std::function glue) */
typedef void (*findmy_msg_handler)(void *ctx, const char *topic, const json_t *payload);

void findmy_ctor(findmy_strategy *self, void *publisher, void *a,
                 void *storage, ux_service *ctx);   /* 0x13b820 */
void findmy_toggle(findmy_strategy *self);          /* 0x13a7e0 */
void findmy_publish_certified(findmy_strategy *self);/* 0x139d00 */

#endif /* UX_FINDMY_H */
