#ifndef UX_RESET_H
#define UX_RESET_H

#include "ux_common.h"

/*
 * reset.cpp — Reset (factory-reset-by-button-hold) strategy.
 * program "ux", AArch64, base 0x100000.
 *
 * Reconstructed from Reset::OnPress 0x178c10 and Reset::OnRelease 0x178d70
 * (recovered from an undisassembled indirect-dispatch gap). The clock source
 * (virtual vt+0x10) and the press-event vector / timer scheduler are vendor.
 */
typedef struct reset_strategy {
    void   *vtable;        /* [0]  +0x00  clock source (vt+0x10 = now) */
    void   *scheduler;     /* [1]  +0x08  press-event / reboot scheduler */
    long    deadline;      /* [2]  +0x10  press timestamp + hold threshold */
    long    hold_ms;       /* [3]  +0x18  long-press hold duration */
} reset_strategy;

void reset_on_press(reset_strategy *self);    /* 0x178c10 */
void reset_on_release(reset_strategy *self);  /* 0x178d70 */

#endif /* UX_RESET_H */
