/*
 * od_table.c — VanMoof `vm` Object-Dictionary table (reconstructed)
 *
 * OEM: /usr/bin/power
 *   od_key_cmp        0x158260   (raw thunk; logic VERIFIED via objdump)
 *   vm_od_table_scan  0x158860
 *   vm_od_table_add   0x158670
 *
 * Faithful translation of the decompiled logic.
 *
 * RX path (OEM vm_tp_handle_frame 0x157d30): builds key = a0 | a1<<8 | a2<<16,
 * calls vm_od_table_scan, then dispatches by entry->kind:
 *   kind 0  single-frame: hand the inline <=8B payload to entry->cb
 *   kind 1  polled/request: semaphore-gated; the 0x10 bit of id a3 is the
 *           request/response direction flag
 *   kind 2  multi-frame TP: DLC<9 inline, else reassembled in a separate
 *           100-slot pool (vm_s+0x2858) before calling entry->cb
 */
#include "od_table.h"

/* OEM 0x158260. The frame key {a0,a1,...} vs the entry's (a0,a1). */
int od_key_cmp(const void *key, const void *entry)
{
    const uint8_t *k = key;
    const struct od_descriptor *e = entry;
    return (k[0] != e->a0) || (k[1] != e->a1);   /* 0 == match */
}

/* OEM 0x158860. */
struct od_descriptor *vm_od_table_scan(struct od_table *t, const void *key)
{
    size_t i;

    if (!t || !key)
        return NULL;

    for (i = 0; i < t->count; i++) {
        struct od_descriptor *e = &t->base[i];
        if (t->cmp(key, e) == 0)
            return e;
    }
    return NULL;
}

/* OEM 0x158670. */
int vm_od_table_add(struct od_table *t, const struct od_descriptor *tmpl)
{
    size_t i;

    if (!t || !tmpl)
        return 1;
    if (t->count >= t->cap)
        return 4;

    /* reject a duplicate address (cmp keys on a0,a1) */
    for (i = 0; i < t->count; i++) {
        if (t->cmp(&tmpl->a0, &t->base[i]) == 0)
            return 2;
    }

    t->base[t->count++] = *tmpl;     /* copy the 0x50-byte descriptor */
    return 0;
}

/*
 * Per-signal registration thunk pattern (OEM e.g. reg_thunk_battery_voltage
 * 0x13be80). Each signal has its own thunk that bakes the vm_address as a
 * compile-time immediate, then appends the descriptor. Modelled here:
 */
int vm_od_register(struct od_table *t,
                   uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3,
                   uint8_t kind, uint8_t dlc,
                   void (*cb)(void *, const struct vm_frame *), void *ctx)
{
    struct od_descriptor d = {0};
    d.a0 = a0; d.a1 = a1; d.a2 = a2; d.a3 = a3;
    d.kind = kind; d.dlc = dlc;
    d.cb = cb; d.ctx = ctx;
    return vm_od_table_add(t, &d);
}

/*
 * The primary (Panasonic) battery is node a0=0xA4, a2=0x82, a3=0x00, kind=2.
 * The static map (OEM register thunks), for reference — see can-bus.md §4:
 *
 *   a1   signal       dlc   nominal can_id
 *   0x03 charging      5    0x14807040
 *   0x04 cell          5    0x14809040   (no-op decoder)
 *   0x05 capacity/soc  7    0x1480B040
 *   0x06 warning       6    0x1480D040
 *   0x07 status        3    0x1480F040
 *   0x08 voltage       8    0x14811040
 *   0x09 temperature  0x1a  0x14813040
 *   0x0A health        8    0x14815040
 */
