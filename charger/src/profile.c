/*
 * profile.c — charge-current derating and charge-profile setpoint search.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). The charge-current limit is
 * derated from a base value by the pack voltage and a temperature breakpoint
 * table; the charge-profile search finds a timing divisor and packs the setpoint
 * frame. Translated from the OEM image (raw ARM Cortex-M, base 0x0). The
 * temperature table is a flash calibration constant living beyond this image
 * slice (CHG_TEMP_TABLE = 0x7e64); it is declared extern and satisfied at link.
 */

#include "charger.h"

/* Off-image temperature breakpoint table (11 entries) at CHG_TEMP_TABLE. */
#define CHG_TEMP_TABLE_REF ((const charge_temp_bp_t *)CHG_TEMP_TABLE)

/* Shared derating: base limit -> voltage derate -> temperature-table derate.
 * The two OEM functions differ only in which base-limit byte they read. */
static uint32_t charge_derate(uint32_t limit)
{
    uint16_t voltage = *(volatile uint16_t *)(CHG_TELEM + 2);
    const charge_temp_bp_t *table = CHG_TEMP_TABLE_REF;
    uint32_t vderate;
    int16_t temp;
    uint16_t idx;
    int32_t tderate;

    if (voltage < 200)
        vderate = 0;
    else
        vderate = ((uint32_t)*(volatile uint8_t *)CHG_TELEM_SCALE *
                   (((uint32_t)voltage * 28000 + 0x8000) >> 16)) >> 13;

    temp = *(volatile int16_t *)(CHG_TEMP_STRUCT + 4);
    if (vderate < limit)
        limit = (limit - vderate) & 0xffff;

    if (temp < -10) {
        idx = 0;
    } else {
        idx = 10;
        do {
            idx = (uint16_t)(idx - 1);
            if (idx == 0xffff)
                break;
        } while (temp < table[idx].thresh);
    }

    tderate = ((table[idx].slope * temp + table[idx].offset) >> 16) * 0x22ee >> 16;
    if (tderate < 1 || tderate < (int32_t)limit)
        limit = (limit - tderate) & 0xffff;
    return limit;
}

/* charger_compute_charge_current_limit — derated limit from base CHG_LIMIT_A.
 *
 * OEM disassembly (0x000019b0..0x000019d2):
 */
uint32_t charger_compute_charge_current_limit(void)
{
    return charge_derate(*(volatile uint8_t *)CHG_LIMIT_A);
}

/* charger_compute_charge_current_setpoint — derated limit from base CHG_TELEM.
 *
 * OEM disassembly (0x000019d4..0x00001a06):
 *
 * Identical to charger_compute_charge_current_limit but reads the base from the
 * telemetry struct byte 0 instead of CHG_LIMIT_A.
 */
uint32_t charger_compute_charge_current_setpoint(void)
{
    return charge_derate(*(volatile uint8_t *)CHG_TELEM);
}

/* charger_compute_charge_profile_setpoints — find the charge timing divisor and
 * pack the setpoint frame.
 *
 * OEM disassembly (0x00000814..0x000008ab):
 *
 * Searches the period multiplier from 0x181 down to 3 for one that exactly
 * divides `total` (period = base * mult must divide total). On a hit it writes
 * the quotient-1 (u16) at frame[0], rejects if >= 0x200, then selects a current
 * factor by the base vs the two thresholds (0xf423f / 0xc34ff) — 0x2ee / 800 /
 * 0x36b — derives the duty bytes ((mult*factor)/1000 - 1) at frame[3] and the
 * complementary byte at frame[4], a phase byte at frame[2], and returns 1.
 * Returns 0 if no divisor yields an in-range frame.
 */
int charger_compute_charge_profile_setpoints(uint32_t base, uint32_t total, void *frame)
{
    uint8_t  *f = (uint8_t *)frame;
    uint32_t mult = 0x181;
    uint32_t period = base * 0x181;

    do {
        if (period <= total && period * (total / period) - total == 0) {
            uint32_t q = total / period - 1;
            int32_t  factor;
            uint32_t duty;
            uint8_t  duty_lo;

            *(uint16_t *)(f + 0) = (uint16_t)q;
            if ((q & 0xffff) > 0x1ff)
                return 0;

            if (CHG_PROFILE_THRESH_HI < base)
                factor = 0x2ee;
            else if (CHG_PROFILE_THRESH_LO < base)
                factor = 800;
            else
                factor = 0x36b;

            duty = (mult * (uint32_t)factor) / 1000 - 1;
            duty_lo = (uint8_t)(duty & 0xff);
            f[3] = (uint8_t)duty;

            if (duty_lo <= (uint8_t)((mult & 0xff) - 3)) {
                int32_t comp = (int32_t)((mult - 3) - duty_lo);
                f[4] = (uint8_t)comp;
                if ((int32_t)(comp << 24) >= 0) {
                    f[2] = (uint8_t)(((mult & 0xff) + 4) / 5 - 1);
                    return 1;
                }
            }
        }
        mult -= 1;
        period -= base;
        if (mult == 2)
            return 0;
    } while (1);
}
