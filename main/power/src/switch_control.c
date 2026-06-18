/*
 * switch_control.c — power switch matrix + gpio/charger backends
 *
 * OEM: /usr/bin/power, devices/main/power/src/switch_control.cpp
 *   gpio::gpio (ctor)                 0x1477b0
 *   gpio::write_value                 0x147d60
 *   gpio::read_value                  0x148070
 *   gpio_switch::get (active-low)     0x142bb0
 *   gpio_switch::on                   0x142920
 *   gpio_switch::off                  0x142900
 *   gpio_switch::gpio_switch (ctor)   0x142930
 *   charger_switch::on                0x1431e0
 *   charger_switch::off               0x142ff0
 *   charger_switch::gpio_switch(ctor) 0x1433d0
 *   charger_switch::pulse             0x14351c
 *   buck_set_enable                   0x11d920
 *   switch_control::set_sw3_for_state 0x11e2b0
 *   switch_control::set_switches...   0x11e430
 *
 * Faithful translation of the decompiled logic. All sysfs paths, the
 * direction/value semantics and the per-state switch table are taken verbatim
 * from the binary. The C++ stream I/O, the std::string name buffer, and the
 * std::function CAN relay are modelled through power_common.h helpers
 * (sysfs_write_str / sysfs_read_long / common_logf) rather than byte-rebuilt;
 * the emitted filesystem accesses are identical to the OEM.
 */
#define _DEFAULT_SOURCE          /* nanosleep() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "switch_control.h"

#define SC_FILE  "devices/main/power/src/switch_control.cpp"

/* ----------------------------------------------------------------------- *
 * gpio class — kernel /sys/class/gpio backend (vtable @0x184660)
 * ----------------------------------------------------------------------- */

/* OEM 0x1477b0 — gpio::gpio(line, direction).
 * Builds /sys/class/gpio/gpio<line>; if the pin is not yet exported, writes
 * <line> to .../export and the direction ("out" for 0, "in" for 1) to
 * .../gpio<line>/direction. direction: 0 = output, 1 = input. */
void gpio_ctor(gpio *self, int line, int direction)
{
    char dir_path[64];

    self->vtable = (void *)0x184660;   /* OEM gpio vtable */
    self->line   = line;

    /* If /sys/class/gpio/gpioN already exists the pin is exported -> no-op. */
    snprintf(dir_path, sizeof dir_path, "/sys/class/gpio/gpio%d", line);
    if (access(dir_path, F_OK) != 0) {
        char num[16];
        snprintf(num, sizeof num, "%d", line);
        sysfs_write_str("/sys/class/gpio/export", num);

        {
            char dpath[80];
            snprintf(dpath, sizeof dpath,
                     "/sys/class/gpio/gpio%d/direction", line);
            /* OEM writes "out" (len 3) for dir 0, "in" (len 2) otherwise. */
            sysfs_write_str(dpath, direction == 0 ? "out" : "in");
        }
    }
}

/* OEM 0x147d60 — open ofstream on /sys/class/gpio/gpio<line>/value and write
 * the boolean level. (The OEM passes (param==1) so any non-zero -> '1'.) */
void gpio_write_value(gpio *self, int level)
{
    char path[80];

    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/value", self->line);
    sysfs_write_str(path, level ? "1" : "0");
}

/* OEM 0x148070 — open ifstream on /sys/class/gpio/gpio<line>/value, read an
 * int, return value != 0. */
bool gpio_read_value(gpio *self)
{
    char path[80];
    long v;

    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/value", self->line);
    v = sysfs_read_long(path);
    return v != 0;
}

/* ----------------------------------------------------------------------- *
 * gpio_switch wrapper (vtable @0x184560)
 * ----------------------------------------------------------------------- */

/* OEM 0x142930 — gpio_switch(line, init_mode, active_low).
 * Underlying gpio is created as an output; init_mode 0 asserts, 1 de-asserts. */
void gpio_switch_ctor(gpio_switch *self, int line, int init_mode,
                      uint8_t active_low)
{
    self->vtable     = (void *)0x184560;
    gpio_ctor(&self->pin, line, 0 /* output */);
    self->active_low = active_low;

    if (init_mode == 0)
        gpio_write_value(&self->pin, active_low);
    else if (init_mode == 1)
        gpio_write_value(&self->pin, active_low ^ 1);
}

/* OEM 0x142920 — assert: drive the line to its active level. */
void gpio_switch_on(gpio_switch *self)
{
    gpio_write_value(&self->pin, self->active_low);
}

/* OEM 0x142900 — de-assert: drive the line to the inactive level. */
void gpio_switch_off(gpio_switch *self)
{
    gpio_write_value(&self->pin, self->active_low ^ 1);
}

/* OEM 0x142bb0 — active-low-aware getter. */
bool gpio_switch_get(gpio_switch *self)
{
    bool raw = gpio_read_value(&self->pin);
    if (self->active_low)
        return raw == false;
    return raw;
}

/* ----------------------------------------------------------------------- *
 * charger_switch — BQ25672 named gpio (vtable @0x184560 variant)
 * Writes "<name> <0|1>" lines into /sys/bus/i2c/devices/2-006b/gpios.
 * ----------------------------------------------------------------------- */

static void charger_switch_write(charger_switch *self, uint8_t level)
{
    char line[96];
    /* OEM builds: dir-string + name + ' ' + level, then ofstream<<. */
    snprintf(line, sizeof line, "%s %u", self->name, (unsigned)(level & 1));
    sysfs_write_str(BQ25672_GPIOS_DIR, line);
}

/* OEM 0x1431e0 — assert the named charger line. */
void charger_switch_on(charger_switch *self)
{
    charger_switch_write(self, self->level);
}

/* OEM 0x142ff0 — de-assert the named charger line. */
void charger_switch_off(charger_switch *self)
{
    charger_switch_write(self, (uint8_t)(self->level ^ 1));
}

/* OEM 0x14351c — pulse: de-assert, sleep `ms`, re-assert. */
void charger_switch_pulse(charger_switch *self, long ms)
{
    charger_switch_off(self);
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    charger_switch_on(self);
}

/* ----------------------------------------------------------------------- *
 * BUCK enable (the gpio_switch at switch_control+0x40)
 * ----------------------------------------------------------------------- */

/* OEM 0x11d920 — buck_set_enable(switch_ctrl, enable).
 * enable -> assert via gpio_switch_on; disable -> de-assert via gpio_switch_off
 * (the physical inversion is carried by the switch's active_low flag, matching
 * the OEM's enable->off-routine / disable->on-routine call mapping). Logs only
 * on a real change (last state cached at +0xe0). */
void buck_set_enable(switch_control *self, char enable)
{
    const char *label;

    if (enable == 0) {
        gpio_switch_off(&self->buck);
        if (self->buck_state == 0)
            goto store;                 /* no change -> no log */
        label = "DISABLED";
    } else {
        gpio_switch_on(&self->buck);
        if (self->buck_state != 0)
            goto store;                 /* no change -> no log */
        label = "ENABLED";
    }
    common_logf(SC_FILE, 0x52, LOG_INFO, "BUCK %s", label);

store:
    self->buck_state = enable;
}

/* ----------------------------------------------------------------------- *
 * SW3 helper (the model-dependent power switch at +0xa0)
 * ----------------------------------------------------------------------- */

/* The OEM relays the SW3 request to the CAN layer through a std::function
 * installed at switch_control+0x118 (validity at +0x128, invoke via +0x130).
 * Modelled here as an indirect call; the framework binds the real handler. */
typedef void (*sw3_relay_fn)(void *ctx, const uint8_t *a, const uint8_t *b);

/* OEM 0x11e2b0 — set_sw3_for_state(switch_ctrl, on).
 * Builds the per-model relay payload, dispatches it over CAN (if bound), then
 * drives the physical SW3 gpio. Note the inversion: on -> gpio_switch_off,
 * off -> gpio_switch_on. */
void switch_control_set_sw3_for_state(switch_control *self, bool on)
{
    void        *relay_ctx  = *(void **)((uint8_t *)self + 0x128);
    sw3_relay_fn relay_call = *(sw3_relay_fn *)((uint8_t *)self + 0x130);
    void        *relay_obj  = (uint8_t *)self + 0x118;
    char         m0 = self->bike_model[0];
    char         m1 = self->bike_model[1];
    uint8_t      arg0, arg1;

    /* Model 'S5' -> {on, 0}; 'A5' and others -> {on, on}. */
    if (m0 == 'S' && m1 != '\0') {
        arg0 = (uint8_t)on;
        arg1 = 0;
    } else {
        arg0 = (uint8_t)on;
        arg1 = (uint8_t)on;
    }

    if (relay_ctx != NULL)
        relay_call(relay_obj, &arg0, &arg1);
    /* (empty std::function would hit __throw_bad_function_call — dead path.) */

    /* Drive the physical SW3 line (inverted w.r.t. `on`). */
    if (on)
        gpio_switch_off(&self->sw3);
    else
        gpio_switch_on(&self->sw3);
}

/* ----------------------------------------------------------------------- *
 * The per-state switch matrix
 * ----------------------------------------------------------------------- */

/* Cached previous state — only re-log on a transition (OEM DAT_0018dfb0). */
static enum power_state s_prev_state = ST_INVALID;

/* OEM 0x11e430 — set_switches_for_state(switch_ctrl, state).
 * Re-drives SW0/SW1/SW2 (+0x20/+0x60/+0x80), the charger-rail enable (+0xe8)
 * and SW3 (+0xa0) for the requested power_state. */
void switch_control_set_switches_for_state(switch_control *self,
                                           enum power_state state)
{
    if ((int)state != (int)s_prev_state) {
        common_logf(SC_FILE, 0x9a, LOG_INFO,
                    "Set switches for state %s", power_state_name(state));
        s_prev_state = state;
    }

    if ((int)state < 7) {
        if ((int)state < 3) {
            if (state == ST_SHIPPING) {            /* 1 */
                gpio_switch_on(&self->sw0);
                gpio_switch_on(&self->sw1);
            } else if (state == ST_STANDBY) {      /* 2 */
                gpio_switch_off(&self->sw0);
                gpio_switch_off(&self->sw1);
            } else {
                return;                            /* ST_INVALID: no change */
            }
            gpio_switch_on(&self->sw2);
            charger_switch_off(&self->charger);    /* +0xe8 de-asserted */
            switch_control_set_sw3_for_state(self, false);
        } else {                                   /* OPERATIONAL..ALARM 3..6 */
            gpio_switch_off(&self->sw0);
            gpio_switch_off(&self->sw1);
            gpio_switch_off(&self->sw2);
            charger_switch_on(&self->charger);     /* +0xe8 asserted */
            switch_control_set_sw3_for_state(self, true);
        }
    } else if (state == ST_MAINTENANCE) {          /* 7: all switches on */
        gpio_switch_on(&self->sw0);
        gpio_switch_on(&self->sw1);
        gpio_switch_on(&self->sw2);
        gpio_switch_on(&self->sw3);
    }
}
