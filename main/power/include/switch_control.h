/*
 * switch_control.h — power switch matrix + gpio backends (reconstructed)
 *
 * OEM: /usr/bin/power, devices/main/power/src/switch_control.cpp
 *
 * Two switch backends drive the bike's power rails:
 *   - gpio_switch    : a kernel pin under /sys/class/gpio/gpioN
 *   - charger_switch : a named line on the BQ25672 charger, written through
 *                      /sys/bus/i2c/devices/2-006b/gpios
 * switch_control owns four power switches (SW0..SW3), a BUCK enable switch and
 * a charger-rail enable, and re-drives them all whenever the power_state
 * changes (set_switches_for_state).
 */
#ifndef SWITCH_CONTROL_H
#define SWITCH_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "power_common.h"

/* ---- gpio class (kernel /sys/class/gpio backend, vtable @0x184660) ------- */
/* Object is offset-based in the OEM image; we mirror the byte layout. */
typedef struct gpio {
    void   *vtable;       /* +0x00 : OEM @0x184660                          */
    int32_t line;         /* +0x08 : gpio pin number                        */
    uint8_t _pad[12];     /* +0x0c : (padding to 24 bytes)                  */
} gpio;

void gpio_ctor(gpio *self, int line, int direction); /* dir: 0=out, 1=in */
void gpio_write_value(gpio *self, int level);         /* writes '0'/'1' to /value */
bool gpio_read_value(gpio *self);                     /* reads /value -> bool */

/* ---- gpio_switch wrapper (vtable @0x184560 variant) --------------------- */
typedef struct gpio_switch {
    void   *vtable;       /* +0x00 */
    gpio    pin;          /* +0x08 : underlying gpio (line at switch+0x10)  */
    uint8_t active_low;   /* +0x18 : asserted level                        */
} gpio_switch;

void gpio_switch_ctor(gpio_switch *self, int line, int init_mode, uint8_t active_low);
void gpio_switch_on (gpio_switch *self);   /* assert   : write active_low      */
void gpio_switch_off(gpio_switch *self);   /* de-assert: write active_low ^ 1  */
bool gpio_switch_get(gpio_switch *self);   /* active-low-aware line state      */

/* ---- charger_switch (BQ25672 named gpio, vtable @0x184560 variant) ------ */
/* The OEM object embeds a std::string name; modelled here as a C string. */
typedef struct charger_switch {
    void   *vtable;       /* +0x00 */
    char   *name;         /* +0x08 : line name (e.g. "pwr-good")           */
    uint8_t level;        /* +0x28 : asserted level                        */
} charger_switch;

void charger_switch_on   (charger_switch *self);          /* assert            */
void charger_switch_off  (charger_switch *self);          /* de-assert         */
void charger_switch_pulse(charger_switch *self, long ms); /* off, sleep, on    */

/* I2C charger gpio control directory (BQ25672). */
#define BQ25672_GPIOS_DIR  "/sys/bus/i2c/devices/2-006b/gpios"

/* ---- switch_control object --------------------------------------------- *
 * PowerService + 0x208. Only the fields touched by this TU are named; the
 * rest is reserved so the byte offsets match the OEM. */
typedef struct switch_control {
    uint8_t        _hdr[0x20];     /* +0x000 : vtable + book-keeping        */
    gpio_switch    sw0;            /* +0x020 : power switch 0               */
    gpio_switch    buck;           /* +0x040 : BUCK enable                  */
    gpio_switch    sw1;            /* +0x060 : power switch 1               */
    gpio_switch    sw2;            /* +0x080 : power switch 2               */
    gpio_switch    sw3;            /* +0x0a0 : power switch 3 (model dep.)  */
    uint8_t        _gap0[0x40 - sizeof(gpio_switch)]; /* fill +0xa0..+0xe0  */
    uint8_t        buck_state;     /* +0x0e0 : last BUCK enable (log dedup) */
    uint8_t        _gap1[7];       /* +0x0e1                                */
    charger_switch charger;        /* +0x0e8 : charger-rail enable          */
    uint8_t        _gap2[0x118 - 0xe8 - sizeof(charger_switch)];
    uint8_t        sw3_relay[0x20];/* +0x118 : std::function relay (CAN)    */
    char           bike_model[2];  /* +0x138 : "A5" / "S5"                  */
} switch_control;

void switch_control_set_switches_for_state(switch_control *self, enum power_state state);
void switch_control_set_sw3_for_state    (switch_control *self, bool on);

/* buck_set_enable: PowerService+0x208 view (the switch_control object). */
void buck_set_enable(switch_control *self, char enable);

#endif /* SWITCH_CONTROL_H */
