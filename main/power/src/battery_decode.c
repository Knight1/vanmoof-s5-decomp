/*
 * battery_decode.c — primary (Panasonic) battery OD payload decoders
 *
 * OEM: /usr/bin/power, monitor.cpp
 *   soc_to_soc_app            0x12cf80
 *   battery_voltage_decode    0x1285c0
 *   battery_charging_decode   0x128730
 *   battery_health_decode     0x1289c0
 *   battery_capacity_decode   0x12d150
 *   battery_temperature_decode 0x12da90
 *
 * Faithful translation of the decompiled logic. The publish topics and field
 * offsets are taken verbatim from the binary. Values are little-endian.
 */
#include "battery_decode.h"

#define RD16(p, off) ((uint16_t)((p)[off] | ((uint16_t)(p)[(off) + 1] << 8)))

/* temperature publishes a 4-field JSON object; the JSON build is vendor (std). */
extern void od_pub_temp_json(void *ipc, const char *topic,
                             uint8_t cell1, uint8_t cell2,
                             uint8_t chg_mos, uint8_t dsg_mos);

/*
 * OEM 0x12cf80 — map the raw state-of-charge byte to the displayed SoC via a
 * piecewise-linear curve. The OEM builds a std::map from the inline table
 *   raw: 0  4  7 12 24 33 80 89 97 100
 *   app: 0  0  3  6 11 21 81 91 100 100
 * then linearly interpolates between the two breakpoints bracketing `raw`.
 * (e.g. raw 4% -> 0% cutoff; raw 80% -> 81%.)
 */
uint8_t soc_to_soc_app(uint8_t raw_soc)
{
    static const uint8_t bp[10][2] = {
        {  0,   0}, {  4,   0}, {  7,   3}, { 12,   6}, { 24,  11},
        { 33,  21}, { 80,  81}, { 89,  91}, { 97, 100}, {100, 100},
    };
    int i;

    for (i = 0; i < 10; i++) {
        if (raw_soc <= bp[i][0]) {
            int x0, y0, x1, y1, dx, interp;
            if (i == 0)
                return bp[0][1];
            x0 = bp[i - 1][0]; y0 = bp[i - 1][1];
            x1 = bp[i][0];     y1 = bp[i][1];
            dx = x1 - x0;
            interp = dx ? (y1 - y0) * ((int)raw_soc - x0) / dx : 0;
            return (uint8_t)(y0 + interp);
        }
    }
    return 0;   /* raw > 100: OEM falls off the map end and returns 0 */
}

/* OEM 0x1285c0. voltage = u16[0:2] (mV); cached for the power calc. */
void battery_voltage_decode(struct battery_monitor *m, const uint8_t *p)
{
    m->voltage = RD16(p, 0);
    od_pub_u16(m->ipc, "power/battery/primary/info/voltage", RD16(p, 0), 0, 0);
}

/* OEM 0x128730. */
void battery_charging_decode(struct battery_monitor *m, const uint8_t *p)
{
    /* OEM first calls (this+0x48 vtable+0x18)(...,0): a charging-session reset. */
    m->charge_current = RD16(p, 0);
    od_pub_u16(m->ipc, "power/battery/primary/info/charge_voltage", RD16(p, 2), 0, 0);
    od_pub_u16(m->ipc, "power/battery/primary/info/charge_current", RD16(p, 0), 0, 0);
}

/* OEM 0x1289c0. */
void battery_health_decode(struct battery_monitor *m, const uint8_t *p)
{
    od_pub_u16(m->ipc, "power/battery/primary/info/health", RD16(p, 4), 0, 0);
    od_pub_u16(m->ipc, "power/battery/primary/info/cycles", RD16(p, 6), 0, 0);
}

/* OEM 0x12d150. soc (raw u16, retained) + soc_app (display curve, retained). */
void battery_capacity_decode(struct battery_monitor *m, const uint8_t *p)
{
    m->soc = RD16(p, 0);
    od_pub_u16(m->ipc, "power/battery/primary/info/soc", RD16(p, 0), 4, 1);
    od_pub_u16(m->ipc, "power/battery/primary/info/soc_app", soc_to_soc_app(p[0]), 4, 1);
}

/* OEM 0x12da90. One frame carries 4 temperatures + currents. */
void battery_temperature_decode(struct battery_monitor *m, const uint8_t *p)
{
    uint16_t discharge_current = RD16(p, 4);
    uint16_t max_current       = RD16(p, 6);
    float power;

    /* {"cell 1": p0, "cell 2": p1, "chg mos": p2, "dsg mos": p3} */
    od_pub_temp_json(m->ipc, "power/battery/primary/info/temperature",
                     p[0], p[1], p[2], p[3]);

    od_pub_double(m->ipc, "power/battery/primary/info/discharge_current",
                  (double)discharge_current, 0, 0);
    od_pub_double(m->ipc, "power/battery/primary/info/max_current",
                  (double)max_current, 0, 0);

    /* power = (discharge_current / 1000) * (voltage / 1000)  [W] */
    power = ((float)discharge_current / 1000.0f) * ((float)m->voltage / 1000.0f);
    od_pub_float(m->ipc, "power/battery/primary/info/power", power, 0, 0);
}

/*
 * Not reconstructed here (bit-field decoders with ~40 / ~20 runtime-named
 * boolean topics, no clean numeric form):
 *   battery_status_decode  0x1268d0  — payload[0..6] flags; charging flag
 *                                      = p[0]>>7, state nibble = p[6]&0x0F
 *   battery_warning_decode 0x1275a0  — payload[0..4] alarm flags
 *   battery_cell_decode    0x12e2a0  — no-op stub (`return 0`)
 */
