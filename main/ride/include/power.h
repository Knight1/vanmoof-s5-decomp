/*
 * power.h — module-local model for the reconstructed VanMoof S5 i.MX8 `ride`
 * service IPower sensor (program "ride", base 0x100000). Included AFTER
 * ride_common.h. Models common::IPower (devices/main/ride/src/power.cpp): a
 * thread-safe holder for battery temperature / charge state / capacity (SOC),
 * fed by three CANopen OD callbacks, with an SOC/temperature-derated
 * max-discharge-current computation.
 *
 * Object layout (OEM 0x80 bytes), field offsets verified against the OEM:
 *   +0x70  uint16  max-discharge current (computed; "known" running value)
 *   +0x72  uint16  capacity input to the derate (raw OD capacity word)
 *   +0x74  int16   temperature low half (buf[4..5])
 *   +0x76  uint16  state-of-charge (SOC, percent)
 *   +0x78  bool    powered_on   (state in {1,2})
 *   +0x79  bool    charging     (state in {3,4})
 *   +0x7a  bool    sufficient_soc
 *
 * NOTE on the temperature/capacity pair: the OD temperature callback writes a
 * u32 across +0x72 (high half from buf[6..7]) and +0x74 (low half from
 * buf[4..5]); computeMaxDischargeCurrent reads the u16 at +0x72 as the capacity
 * to derate. We model that word as `cap_word` and keep the low half as
 * `temperature_lo`.
 */
#ifndef RIDE_POWER_H
#define RIDE_POWER_H

#include "ride_common.h"

typedef struct power_sensor {
    void    *vtbl_state;       /* +0x00 common::StateClient vtable */
    void    *vtbl_pure;        /* +0x08 */
    uint8_t  state_obj[0x40];  /* +0x10 base/state-client subobject (OD glue) */
    ipower  *self;            /* +0x50 owning IPower / mqtt-state ptr */
    void    *od_entries[3];    /* +0x58 OD-entry vector {begin,cur,end} */
    uint16_t max_discharge;    /* +0x70 */
    uint16_t cap_word;         /* +0x72 (temperature hi half / derate input) */
    int16_t  temperature_lo;   /* +0x74 */
    uint16_t soc;              /* +0x76 */
    bool     powered_on;       /* +0x78 */
    bool     charging;         /* +0x79 */
    bool     sufficient_soc;   /* +0x7a */
} power_sensor;

/* OD-payload shapes (CANopen entries delivered by od_notify). */
typedef struct power_od_temperature { uint8_t pad[4]; int16_t lo; int16_t hi; } power_od_temperature;
typedef struct power_od_state       { int8_t  state; } power_od_state;
typedef struct power_od_capacity    { uint16_t soc; } power_od_capacity;

/* OEM-named API (power.cpp). */
void     power_ctor(power_sensor *self, ipower *owner, od_registry *od);
unsigned power_compute_max_discharge_current(power_sensor *self);
bool     power_is_powered_on(const power_sensor *self);
bool     power_is_charging(const power_sensor *self);
bool     power_is_sufficient_soc(const power_sensor *self);
void     power_set_temperature(power_sensor *self, uint16_t hi, int16_t lo);
void     power_set_capacity(power_sensor *self, uint16_t soc);
void     power_reset(power_sensor *self);
int      power_od_temperature_callback(power_sensor *self, void *unused, const power_od_temperature *p);
int      power_od_state_callback(power_sensor *self, void *unused, const power_od_state *p);
int      power_od_capacity_callback(power_sensor *self, void *unused, const power_od_capacity *p);

#endif /* RIDE_POWER_H */
