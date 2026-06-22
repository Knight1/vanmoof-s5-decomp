/*
 * dispatch.c — charger CAN-register event dispatchers.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). Four near-identical
 * dispatchers look an M_CAN peripheral BASE ADDRESS up in the key table
 * (charger_find_fault_index) and call the registered handler from a runtime
 * function-pointer table with the matched key + a RAM argument. Translated from
 * the OEM image (raw ARM Cortex-M, base 0x0). The handler table (RAM, populated
 * elsewhere) and the off-image key table are satisfied at link; this is the
 * charger's CAN-event dispatch glue.
 *
 * NOTE: the index is keyed by the peripheral base address itself (passed as a
 * literal-pool constant) — there is NO MMIO read here. The OEM passes
 * 0x4008_6000 / 0x4008_9000 / 0x4008_a000 / 0x4009_f000 directly to
 * charger_find_fault_index, which scans the key table at CHG_DISP_KEYS.
 */

#include "charger.h"

typedef void (*charge_disp_fn)(uint32_t, uint32_t);

/* Shared dispatch: find the base address's index, call handler[idx](key,arg). */
static void charge_dispatch_from_reg(uint32_t reg_base)
{
    const charge_disp_fn *handlers = (const charge_disp_fn *)CHG_DISP_HANDLERS;
    const uint32_t       *keys     = (const uint32_t *)CHG_DISP_KEYS;
    const uint32_t       *arg1     = (const uint32_t *)CHG_DISP_ARG1;
    int idx = charger_find_fault_index((int)reg_base);

    handlers[idx](keys[idx], arg1[idx]);
}

/* OEM disassembly: 0x00003ea4 / 0x00003f34 / 0x00003f64 / 0x00004024 — identical
 * but for the M_CAN base-address key constant. */
void charger_dispatch_can_86000(void) { charge_dispatch_from_reg(0x40086000u); }
void charger_dispatch_can_89000(void) { charge_dispatch_from_reg(0x40089000u); }
void charger_dispatch_can_8a000(void) { charge_dispatch_from_reg(0x4008a000u); }
void charger_dispatch_can_9f000(void) { charge_dispatch_from_reg(0x4009f000u); }
