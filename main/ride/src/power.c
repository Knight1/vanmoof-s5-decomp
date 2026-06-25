/*
 * power.c — reconstructed VanMoof S5 i.MX8 `ride` service IPower sensor.
 * Program "ride" (AArch64, image base 0x100000). Source path quoted by the
 * binary: devices/main/ride/src/power.cpp.
 *
 * Holds battery temperature / charge-state / capacity (SOC) sampled from three
 * CANopen object-dictionary entries, and derives a max-discharge-current that
 * is derated by SOC band and by the temperature-derated capacity word. The
 * OD-vector bookkeeping and runtime_error throw paths in the ctor are vendor
 * STL/state-client glue and are modelled at the call site (od_register).
 */
#include "ride_common.h"
#include "power.h"

#define POWER_SRC "devices/main/ride/src/power.cpp"

/* OEM 0x119060 — IPower ctor. Registers three CANopen OD entries against the
 * three change callbacks below. Field defaults are zeroed; SufficientSOC starts
 * true (param_1[0x7a] = 1). The decompiled body is dominated by std::string and
 * std::vector growth plus a "Failed on a VM call '<name>': <rc>" runtime_error
 * path on registration failure — modelled here by od_register(). */
void power_ctor(power_sensor *self, ipower *owner, od_registry *od)
{
    self->self           = owner;
    self->od_entries[0]  = 0;
    self->od_entries[1]  = 0;
    self->od_entries[2]  = 0;
    self->max_discharge  = 0;
    self->cap_word       = 0;
    self->temperature_lo = 0;
    self->soc            = 0;
    self->powered_on     = false;
    self->charging       = false;
    self->sufficient_soc = true;   /* param_1[0x7a] = 1 */

    od_register(od, "battery_primary_battery_temperature",
                (od_cb)(void(*)(void))power_od_temperature_callback, self);
    od_register(od, "power_control_state",
                (od_cb)(void(*)(void))power_od_state_callback, self);
    od_register(od, "battery_primary_battery_capacity",
                (od_cb)(void(*)(void))power_od_capacity_callback, self);
}

/* OEM 0x118d00 — computeMaxDischargeCurrent.
 * SOC bands (capacity/SOC at +0x76):
 *   SOC <  5    : derate target = 0                       (no current)
 *   5 <= SOC<18 : derate target = cap_word(+0x72) * 0.70
 *   18<= SOC<22 : derate target = cap_word(+0x72) * 0.85
 *   SOC >= 22   : derate target = cap_word(+0x72)         (full)
 * The running value at +0x70 ratchets: once it is >= 0x32 (50) it is only
 * allowed to decrease toward the new target by at most (cur-0x32); below 0x32 it
 * snaps. Logs power.cpp:0x6b (WARN). SufficientSOC (+0x7a) is set when SOC>=0x0d
 * (13). Returns the resulting current. */
unsigned power_compute_max_discharge_current(power_sensor *self)
{
    uint16_t soc = self->soc;
    unsigned target = 0;

    if (soc >= 5) {
        if (soc < 0x12) {
            target = (unsigned)(int)((float)self->cap_word * 0.7f) & 0xffff;
        } else {
            target = (unsigned)self->cap_word;
            if (soc <= 0x15)
                target = (unsigned)(int)((float)target * 0.85f) & 0xffff;
            /* soc >= 0x16: full cap_word, no factor */
        }
    }
    target &= 0xffff;

    /* ratchet into +0x70 — and (OEM) log ONLY on the target<known branch. */
    if (target < self->max_discharge) {
        if (self->max_discharge >= 0x32) {
            unsigned floor_v = (unsigned)(self->max_discharge - 0x32);
            if (target <= (floor_v & 0xffff))
                target = floor_v;
        }
        common_logf(POWER_SRC, 0x6b, LOG_WARN,
                    "max discharge current ma derate %d, known %d, soc %d",
                    target, self->max_discharge, soc);
    }
    self->max_discharge = (uint16_t)target;

    if (target == 0) {
        self->sufficient_soc = false;
        if (soc < 0x0d)
            return 0;
        self->sufficient_soc = true;
        return 0;
    }
    if (soc < 0x0d)
        return target;
    if (self->sufficient_soc)
        return target;
    self->sufficient_soc = true;
    return target;
}

/* OEM 0x118c70 */
bool power_is_powered_on(const power_sensor *self)    { return self->powered_on; }
/* OEM 0x118c80 */
bool power_is_sufficient_soc(const power_sensor *self){ return self->sufficient_soc; }
/* OEM 0x118c60 */
bool power_is_charging(const power_sensor *self)      { return self->charging; }

/* OEM 0x118cb0 — set temperature halves, then wake field-change cond-var. */
void power_set_temperature(power_sensor *self, uint16_t hi, int16_t lo)
{
    self->cap_word       = hi;
    self->temperature_lo = lo;
    od_notify(self->state_obj);
}

/* OEM 0x118ce0 — set capacity/SOC, then wake. */
void power_set_capacity(power_sensor *self, uint16_t soc)
{
    self->soc = soc;
    od_notify(self->state_obj);
}

/* OEM 0x118c40 — reset: clears the temperature u32 (+0x72) and powered_on. */
void power_reset(power_sensor *self)
{
    self->cap_word       = 0;
    self->temperature_lo = 0;
    self->powered_on     = false;
}

/* OEM 0x1199d0 — temperature OD callback. If the IPower vtable's setTemperature
 * slot (+0x40) is the canonical power_set_temperature, store inline + notify;
 * otherwise dispatch the override. payload: lo @+4, hi @+6. */
int power_od_temperature_callback(power_sensor *self, void *unused, const power_od_temperature *p)
{
    (void)unused;
    self->cap_word       = (uint16_t)p->hi;
    self->temperature_lo = p->lo;
    od_notify(self->state_obj);
    return 0;
}

/* OEM 0x119980 — state OD callback. powered_on = state in {1,2}, charging = state in {3,4}. */
int power_od_state_callback(power_sensor *self, void *unused, const power_od_state *p)
{
    (void)unused;
    int8_t s = p->state;
    self->powered_on = (uint8_t)(s - 1) < 2;
    self->charging   = (uint8_t)(s - 3) < 2;
    od_notify(self->state_obj);
    return 0;
}

/* OEM 0x119a30 — capacity OD callback. Stores SOC inline + notify (or dispatch
 * override at vtable +0x48). */
int power_od_capacity_callback(power_sensor *self, void *unused, const power_od_capacity *p)
{
    (void)unused;
    self->soc = p->soc;
    od_notify(self->state_obj);
    return 0;
}
