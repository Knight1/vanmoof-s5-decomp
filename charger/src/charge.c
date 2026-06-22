/*
 * charge.c — charger setpoint scaling, status/fault flags, charge-config init.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). The charger derives its
 * hardware DAC/PWM setpoint fields from a requested charge current/voltage via
 * fixed-point scaling, maintains a software status/fault bit-field, and seeds a
 * charge-config struct at boot. Translated from the OEM image (raw ARM Cortex-M,
 * base 0x0). The memset is vendor libc; the scaling/policy is the charger app.
 */

#include "charger.h"

/* charger_charge_state_init — seed the charge-config struct at boot.
 *
 * OEM disassembly (0x000007b0..0x000007d7):
 *
 * Zero-fills a 0x1c-byte config struct, then writes the nominal full-scale value
 * (1e6) into the first two u32 fields, the default charge setpoint word (0x0a03)
 * at offsets 0x10 and 0x16, and the limit code 3 at offsets 0x12 and 0x18.
 */
void charger_charge_state_init(void *cfg)
{
    uint8_t *p = (uint8_t *)cfg;

    mem_set(p, 0, 0x1c);
    *(uint32_t *)(p + 0x00) = CHG_CFG_FULLSCALE;
    *(uint32_t *)(p + 0x04) = CHG_CFG_FULLSCALE;
    *(uint32_t *)(p + 0x08) = 0;
    *(uint8_t  *)(p + 0x0c) = 0;
    *(uint16_t *)(p + 0x10) = CHG_CFG_SETPOINT;
    *(uint8_t  *)(p + 0x12) = 3;
    *(uint16_t *)(p + 0x16) = CHG_CFG_SETPOINT;
    *(uint8_t  *)(p + 0x18) = 3;
}

/* charger_set_charge_setpoint — convert a requested value to the primary HW
 * setpoint field.
 *
 * OEM disassembly (0x00002a54..0x00002a8e):
 *
 * For a non-zero request, rejects out-of-range inputs (the window
 * (raw - 0x4204) mod 2^16 must be <= 0x972c, i.e. raw in [0x4204, 0xd930]) and
 * otherwise scales via (raw << 13) / 0xea83, storing the u16 result at
 * CHG_SETPOINT_CTX+4. A zero request stores 0. Returns 0 on reject, 1 otherwise.
 */
int charger_set_charge_setpoint(int raw)
{
    uint16_t out = 0;

    if (raw != 0) {
        if (((uint32_t)(raw - 0x4204) & 0xffff) > 0x972c)
            return 0;
        out = (uint16_t)(((uint32_t)(raw << 13)) / 0xea83u);
    }
    *(volatile uint16_t *)(CHG_SETPOINT_CTX + 4) = out;
    return 1;
}

/* charger_set_charge_scaled_setpoint — convert a requested value to the
 * secondary HW setpoint field.
 *
 * OEM disassembly (0x00002a90..0x00002abe):
 *
 * Scales requests below 0x7d1 via (raw << 13) / 0xdb6; clamps at-or-above 0x7d1
 * to the saturated code 0x123b. Stores the u16 result at CHG_SETPOINT_CTX+6.
 */
int charger_set_charge_scaled_setpoint(uint32_t raw)
{
    uint16_t out;

    if (raw < 0x7d1u)
        out = (uint16_t)((raw << 13) / 0xdb6u);
    else
        out = 0x123b;
    *(volatile uint16_t *)(CHG_SETPOINT_CTX + 6) = out;
    return 1;
}

/* charger_status_flag_set — OR a bit into the software status/fault word.
 *
 * OEM disassembly (0x00002c68..0x00002c76):
 */
void charger_status_flag_set(uint16_t mask)
{
    volatile uint16_t *status = (volatile uint16_t *)CHG_STATUS_WORD;
    if ((*status & mask) == 0)
        *status |= mask;
}

/* charger_status_flag_clear — clear a bitmask from the status/fault word.
 *
 * OEM disassembly (0x00002c7c..0x00002c8a):
 */
void charger_status_flag_clear(uint16_t mask)
{
    volatile uint16_t *status = (volatile uint16_t *)CHG_STATUS_WORD;
    if ((*status & mask) != 0)
        *status &= (uint16_t)~mask;
}

/* charger_clear_fault_state — reset the charger fault-state global.
 *
 * OEM disassembly (0x00003a84..0x00003a8a):
 *
 * movs r2,#0; ldr r3,=0x20000ac2; strh r2,[r3] — a 16-bit store (strh), so only
 * the u16 at CHG_FAULT_STATE is cleared (not a 32-bit word).
 */
void charger_clear_fault_state(void)
{
    *(volatile uint16_t *)CHG_FAULT_STATE = 0;
}
