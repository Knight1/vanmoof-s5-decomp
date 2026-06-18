/*
 * od_table.h — VanMoof `vm` Object-Dictionary table (reconstructed)
 *
 * OEM: /usr/bin/power. The `vm` library (Cyphal/UAVCAN-style transport,
 * lib/src/tp/tp.c) routes received CAN frames to per-signal handlers via a
 * fixed-capacity, linear-scan table keyed on (a0,a1). Each signal's address is
 * a STATIC compile-time constant baked into a per-signal registration thunk.
 * See main/docs/can-bus.md §3.
 */
#ifndef OD_TABLE_H
#define OD_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include "vm_can.h"

/*
 * One OD entry — 0x50 bytes. Offsets shown are the OEM layout; the comparator
 * keys strictly on (a0,a1) at +0x18/+0x19.
 */
struct od_descriptor {
    uint8_t  _hdr[0x18];     /* +0x00 internal (name/list links)        */
    uint8_t  a0, a1, a2, a3; /* +0x18..+0x1b  vm_address                 */
    uint8_t  kind;           /* +0x1c  TP framing: 0 single,1 polled,2 TP */
    uint8_t  dlc;            /* +0x1d  expected DLC (approx)             */
    uint8_t  _pad1[2];       /* +0x1e                                    */
    void   (*cb)(void *ctx, const struct vm_frame *f); /* +0x20  handler */
    void    *ctx;            /* +0x28                                    */
    uint8_t  _tail[0x20];    /* +0x30..+0x4f                             */
};

/* Table header (OEM at vm_s+0x0; descriptor array at vm_s+0x20, cap 0x80). */
struct od_table {
    size_t  count;                                   /* +0x00 */
    size_t  cap;                                     /* +0x08 (=0x80) */
    int   (*cmp)(const void *key, const void *entry);/* +0x10 (=od_key_cmp) */
    struct od_descriptor *base;                      /* +0x18 (=vm_s+0x20) */
};

/* OEM 0x158260: 2-byte key match on (a0,a1). Returns 0 on match. */
int  od_key_cmp(const void *key, const void *entry);

/* OEM 0x158860: linear scan (stride 0x50); returns the matching entry or NULL.
 * `key` points at a {a0,a1,a2} buffer (the RX key = a0|a1<<8|a2<<16). */
struct od_descriptor *vm_od_table_scan(struct od_table *t, const void *key);

/* OEM 0x158670: dedup (by a0,a1) then append a 0x50-byte descriptor template.
 * Returns 0 ok, 1 bad-arg, 2 duplicate, 4 full. */
int  vm_od_table_add(struct od_table *t, const struct od_descriptor *tmpl);

#endif /* OD_TABLE_H */
