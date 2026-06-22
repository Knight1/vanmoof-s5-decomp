/*
 * bike.h — reconstructed module decls for the VanMoof S5 `ux` service bike
 * helpers (program "ux", AArch64, image base 0x100000). Source file bike.cpp.
 *
 * The bike object aggregates the LED light driver (this+0x750) and the sound
 * queue (this+0xc60), plus a few state fields touched by the reconstructed
 * functions. Offsets mirror the OEM layout.
 */
#ifndef UX_BIKE_H
#define UX_BIKE_H

#include "ux_common.h"

/* The bike-state object (this == param_1 of the helpers, reached through
 * *param_1 from a strategy). Only the fields the reconstructed functions use
 * are modelled; light/sound drivers are opaque sub-managers. */
typedef struct bike_svc {
    void   *light;          /* LED light driver  (this+0x750)  */
    void   *sound;          /* sound queue       (this+0xc60)  */
    int     shipping_mode;  /* this+0x1018 (1 == shipping)     */
    bool    pride_light_on; /* this+0x168  (pride/light flag)  */
    bool    play_fireworks; /* this+0x1020 (armed fireworks)   */
} bike_svc;

/* OEM 0x13f760: set the pride/theme animation from a JSON number/bool arg. */
bool bike_set_pride_animation(bike_svc *self, const json_t *arg);

/* OEM 0x13dea0: play the startup light pattern + sound, selected by
 * (shipping?, success?). */
void bike_play_startup_feedback(bike_svc *self, bool shipping, bool success);

/* LED ring driver entry (this+0x750), FUN_00147cd0 — VENDOR, modelled.
 * (driver, mode, color, brightness, repeat, loop[, extra]). */
void bike_light_pattern(void *light, int mode, int color, int brightness,
                        int repeat, int loop, int extra);

/* Sound queue enqueue (this+0xc60), FUN_001996e0 — VENDOR, modelled. */
void bike_sound_play(void *sound, const char *name, int prio, int volume);

/* Coerce a JSON number/bool theme arg to its enum and test the "enabled"
 * predicate (FUN_00144400 / FUN_00144f50) — VENDOR json glue, modelled. */
bool bike_theme_is_enabled(const json_t *arg);
int  bike_theme_value     (const json_t *arg);

#endif /* UX_BIKE_H */
