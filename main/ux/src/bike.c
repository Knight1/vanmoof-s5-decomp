/*
 * bike.c — reconstructed VanMoof S5 `ux` service bike helpers.
 * OEM source: devices/main/ux/src/bike.cpp.  Program "ux" (AArch64, base 0x100000).
 *
 * Behaviour-oriented: the json number/bool coercion (FUN_00120a50 /
 * FUN_00144400 / FUN_00144f50), the LED driver (FUN_00147cd0) and the sound
 * queue (FUN_001996e0) are VENDOR and are reached through modelled helpers
 * (declared in bike.h). The control flow, the sound-name literals, the LED
 * pattern constants, and the log string + line number are preserved.
 */
#include "ux_common.h"
#include "bike.h"

/* startup sound names (rodata: bike.cpp string table). */
#define SND_BIKE_STARTUP         "bike_startup"          /* DAT_001b10d0, len 0xc */
#define SND_BIKE_STARTUP_SUCCESS "firmware_update_success"  /* len 0x17 */
#define SND_BIKE_STARTUP_FAILED  "firmware_update_failed"   /* len 0x16 */

/* -------------------------------------------------------------------------
 * bike_set_pride_animation — OEM 0x13f760 (bike.cpp:0x9c)
 * Validates a numeric theme arg (the OEM coerces a json number/bool and range
 * checks it against [0,1] twice). When NOT in shipping mode (this+0x1018 != 1)
 * and the new theme is enabled with the pride light flag (this+0x168) set, it
 * logs WARN "Enabled pride animation, will play fireworks" and arms the
 * fireworks flag (this+0x1020). The resolved theme enum is stored at
 * this+0x1018. Returns true on a valid arg, false otherwise.
 * ----------------------------------------------------------------------- */
bool bike_set_pride_animation(bike_svc *self, const json_t *arg)
{
    int theme;

    /* the arg must be a number/bool (json type tag in {5,6,7}: the
     * `(byte)(*param_2 - 5) < 3` gate). Reject anything else. */
    /* range-validate the coerced value against [0,1] (two coercions in the
     * OEM); modelled by a single value read. */
    theme = bike_theme_value(arg);
    if (theme < 0)
        return false;

    if (self->shipping_mode != 1) {
        if (bike_theme_is_enabled(arg) && self->pride_light_on) {
            common_logf("devices/main/ux/src/bike.cpp", 0x9c, LOG_WARN,
                        "Enabled pride animation, will play fireworks");
            self->play_fireworks = true;        /* this+0x1020 = 1 */
        }
    }

    self->shipping_mode = theme;                /* store enum at this+0x1018 */
    return true;
}

/* -------------------------------------------------------------------------
 * bike_play_startup_feedback — OEM 0x13dea0
 * Plays the startup LED pattern (light driver this+0x750) and a sound (queue
 * this+0xc60) chosen by (shipping?, success?):
 *   shipping  -> pattern(3,0x7a4,0x36,1,1,1) + "bike_startup"
 *   normal    -> pattern(3,0x657,0x3f,1,1)   + "bike_startup"
 *   !shipping, success  -> pattern(3,0x4b1,0x94,1,1) + "firmware_update_success"
 *   !shipping, !success -> pattern(3,0x3ce,0x2f,1,1) + "firmware_update_failed"
 * The (shipping?) branch is selected by this+0x1018 == 1.
 * ----------------------------------------------------------------------- */
void bike_play_startup_feedback(bike_svc *self, bool shipping, bool success)
{
    if (!shipping) {
        if (self->shipping_mode == 1) {
            bike_light_pattern(self->light, 3, 0x7a4, 0x36, 1, 1, 1);
        } else {
            bike_light_pattern(self->light, 3, 0x657, 0x3f, 1, 1, 0);
        }
        bike_sound_play(self->sound, SND_BIKE_STARTUP, 1, 100);
    } else if (!success) {
        bike_light_pattern(self->light, 3, 0x4b1, 0x94, 1, 1, 0);
        bike_sound_play(self->sound, SND_BIKE_STARTUP_SUCCESS, 1, 100);
    } else {
        bike_light_pattern(self->light, 3, 0x3ce, 0x2f, 1, 1, 0);
        bike_sound_play(self->sound, SND_BIKE_STARTUP_FAILED, 1, 100);
    }
}
