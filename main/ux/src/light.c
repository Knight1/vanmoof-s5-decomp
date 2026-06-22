/*
 * light.c — LightService logic for the VanMoof S5 i.MX8 `ux` service.
 * Behaviour reconstruction of devices/main/ux/src/light.cpp.
 * Program "ux" (AArch64, image base 0x100000). OEM addresses in comments.
 *
 * The VM-call registration error path (std::runtime_error build-up), the
 * nlohmann::json variant teardown, and the std::function listener fan-out are
 * VENDOR glue, modelled at the call site, not reconstructed.
 */
#include "ux_common.h"
#include "light.h"

struct light_service {
    void    *vtable;        /* +0x00  PTR_FUN_001fa200 */
    char     _pad0[0xd0 - 0x08];
    void    *drv;           /* +0xd0  LED controller handle */
    char     _pad1[0xf0 - 0xd8];
    int      mode;          /* +0xf0 */
    char     _pad2[0xf8 - 0xf4];
    char     fade_timer[0x1a8 - 0xf8]; /* +0xf8 fade timer object */
    unsigned char brightness;          /* +0x1a8 */
    char     _pad3[0x1ce - 0x1a9];
    unsigned short dark_threshold;     /* +0x1ce */
    unsigned short light_threshold;    /* +0x1d0 */
    char     beam_high;     /* +0x1d2 */
    char     beam_enabled;  /* +0x1d3 */
    char     active;        /* +0x1d4 */
    char     is_dark;       /* +0x1d5 */
    char     _pad4[0x1d8 - 0x1d6];
    int      headlight_state; /* +0x1d8 */
};

/* lux ring-buffer fields live in the vendor object base; accessed via these
 * forward decls so the compiled call structure mirrors the OEM. */
static unsigned short *lux_ring_base (light_service *s) { return *(unsigned short **)((char *)s + 0x1b0); }
static unsigned short *lux_ring_end  (light_service *s) { return *(unsigned short **)((char *)s + 0x1b8); }
static unsigned short  lux_write_idx (light_service *s) { return *(unsigned short  *)((char *)s + 0x1c8); }
static unsigned short  lux_fill_count(light_service *s) { return *(unsigned short  *)((char *)s + 0x1ca); }

/* ---- light_ctor @0x15bbd0 -------------------------------------------------
 * Builds the LightService vtable (PTR_FUN_001fa200), seeds the lux thresholds
 * and brightness, arms the 50ms fade timer (light_brightness_step), registers
 * the VM-call light commands, and subscribes the storage-backed settings.
 */
void light_ctor(light_service *self, void *vm, void *storage)
{
    self->mode            = 3;       /* default: auto (param_1[0x1e] = 3) */
    self->brightness      = 10;      /* default 10% (param_1[0x11] seed) */
    self->dark_threshold  = 0x32;    /* 50  */
    self->light_threshold = 0x8c;    /* 140 */
    self->beam_high       = 0;
    self->beam_enabled    = 0;
    self->active          = 0;
    self->is_dark         = 0;
    self->headlight_state = 0;

    /* VM-call command registration (each throws on failure in OEM). */
    light_vm_register(vm, "frontlight_beam",        (void *)0 /*FUN_0014dc20*/);
    light_vm_register(vm, "frontlight_frame_play",  (void *)0);
    light_vm_register(vm, "frontlight_frame_stop",  (void *)0);
    light_vm_register(vm, "light_threshold",        (void *)0);
    light_vm_register(vm, "rearlight_frame_play",   (void *)0);
    light_vm_register(vm, "rearlight_frame_stop",   (void *)0);
    light_vm_register(vm, "brightness_set",         (void *)0);

    /* storage-backed settings: mode + dark/light thresholds. */
    light_storage_register_default(storage, "mode",            0, self,
                                   (ux_fn)light_on_mode_msg);
    light_storage_register_default(storage, "dark_threshold",  0, self,
                                   (ux_fn)light_on_dark_threshold_msg);
    light_storage_register_default(storage, "light_threshold", 0, self,
                                   (ux_fn)light_on_light_threshold_msg);

    /* arm 50ms periodic fade tick. */
    light_fade_timer_restart(self->fade_timer);
}

/* ---- light_on_light_sample @0x15d820 -------------------------------------
 * Push a new ambient-light reading into the rolling ring buffer, recompute the
 * average, and apply dark/light hysteresis. Crossing below dark_threshold logs
 * "getting dark" and latches is_dark; crossing above light_threshold logs
 * "getting lighter" and clears it. In auto mode (mode==3) drives the headlight.
 */
void light_on_light_sample(light_service *self, unsigned short lux, char force)
{
    unsigned short *base = lux_ring_base(self);
    unsigned short *end  = lux_ring_end(self);
    unsigned long   cap  = (unsigned long)(end - base);
    unsigned short  fill = lux_fill_count(self);
    unsigned long   n;
    unsigned        avg = 0;
    unsigned short  idx;

    /* store sample at the write index. */
    base[lux_write_idx(self)] = lux;

    if (fill < cap) {
        fill++;
        *(unsigned short *)((char *)self + 0x1ca) = fill;
    }
    n = (fill < cap) ? fill : cap;

    if (fill != 0) {
        int sum = 0;
        unsigned short *p;
        for (p = base; p != base + fill; ++p)
            sum += *p;
        if (n != 0)
            avg = (unsigned)((unsigned long)(long)sum / n) & 0xffff;
    }
    *(short *)((char *)self + 0x1cc) = (short)avg;

    /* advance + wrap the write index. */
    idx = (unsigned short)(lux_write_idx(self) + 1);
    if ((unsigned long)lux_write_idx(self) >= cap - 1)
        idx = 0;
    *(short *)((char *)self + 0x1c8) = (short)idx;

    if (self->active == 0)
        return;

    if (avg < self->dark_threshold) {
        if (force != 0 || self->is_dark != 1) {
            common_logf("devices/main/ux/src/light.cpp", 0xb3, LOG_WARN, "getting dark");
            light_listeners_fire(self, (void *)0 /*FUN_001586f0*/);
            self->is_dark = 1;
        } else if (self->mode != 3) {
            return;
        }
    } else if (self->light_threshold < avg) {
        if (self->is_dark == 0 && force == 0) {
            if (self->mode != 3) return;
        } else {
            common_logf("devices/main/ux/src/light.cpp", 0xb9, LOG_WARN, "getting lighter");
            light_listeners_fire(self, (void *)0 /*FUN_00158760*/);
            self->is_dark = 0;
        }
    } else if (self->mode != 3) {
        return;
    }

    /* auto-headlight drive (mode==3 only). */
    if (self->mode != 3)
        return;
    if (force == 0 && self->headlight_state == 3)
        return;

    if (self->is_dark == 0)
        light_headlight_off(self, 0);
    else
        light_headlight_on(self, 0);
}

/* ---- light_headlight_on @0x15d650 ----------------------------------------
 * Turns the headlight on (front/rear brightness 200, high beam) unless already
 * in state 1, then records state 1.
 */
void light_headlight_on(light_service *self, char force)
{
    if (force != 0 || self->headlight_state != 1) {
        unsigned char level = 200;
        light_drv_set_front(self->drv, level);
        light_drv_set_rear (self->drv, level);
        light_apply_beam(self, 1, 1);
        self->headlight_state = 1;
    }
}

/* ---- light_headlight_off @0x15d770 ---------------------------------------
 * Dims the headlight (front/rear brightness 0x32==50, low beam) unless already
 * in state 2, then records state 2.
 */
void light_headlight_off(light_service *self, char force)
{
    if (force != 0 || self->headlight_state != 2) {
        unsigned char level = 0x32;
        light_drv_set_front(self->drv, level);
        light_drv_set_rear (self->drv, level);
        light_apply_beam(self, 0, 1);
        self->headlight_state = 2;
    }
}

/* ---- light_apply_beam @0x15d420 ------------------------------------------
 * Pushes the front+rear LED beam frames for the requested high/low pattern,
 * transitioning if the current beam state differs, then latches beam_enabled
 * and beam_high and restarts the fade timer.
 *
 * Frame words are the verbatim OEM packed descriptors (LED ramp/colour).
 */
void light_apply_beam(light_service *self, unsigned char enabled, char high)
{
    void *drv = self->drv;
    unsigned int  frame_lo;
    unsigned short frame_mid;
    unsigned char  frame_hi;

    if (high == 0) {
        if (self->beam_high != 0) {
            frame_lo = 0x8b060001; frame_mid = 0x2301; frame_hi = 0;
            light_drv_front_beam(drv, &frame_lo);
            frame_lo = 0x5a020001; frame_mid = 0x1e01; frame_hi = 0;
            light_drv_rear_beam(self->drv, &frame_lo);
            if (self->beam_high == 0) goto done;
            drv = self->drv;
        }
        frame_lo = 0x3c020001; frame_mid = 0x0101; frame_hi = 0;
        light_drv_front_beam(drv, &frame_lo);
        frame_mid = 0x0101; frame_lo = 0x3c020001; frame_hi = 0;
        light_drv_rear_beam(self->drv, &frame_lo);
    } else {
        if (self->beam_high == 0) {
            frame_lo = 0x3c060001; frame_mid = 0x4f01; frame_hi = 0;
            light_drv_front_beam(drv, &frame_lo);
            frame_hi = 0; frame_lo = 0x3c020001; frame_mid = 0x1e01;
            light_drv_rear_beam(self->drv, &frame_lo);
            if (self->beam_high != 0) goto done;
            drv = self->drv;
        }
        frame_lo = 0x8b060001; frame_mid = 0x0101; frame_hi = 0;
        light_drv_front_beam(drv, &frame_lo);
        frame_mid = 0x0101; frame_hi = 0; frame_lo = 0x5a020001;
        light_drv_rear_beam(self->drv, &frame_lo);
    }
done:
    /* frame_mid/frame_hi are the decoded high words of each LED beam frame;
     * the modelled light_drv_*_beam write consumes frame_lo. Kept for fidelity. */
    (void)frame_mid;
    (void)frame_hi;
    self->beam_enabled = (char)enabled;
    light_fade_timer_restart(self->fade_timer);
    self->beam_high = high;
}

/* ---- light_brightness_step @0x15d370 -------------------------------------
 * 50ms fade tick: ramps the stored brightness toward its target in ±10 steps
 * (up while beam enabled and < 100, down otherwise), restarts the fade timer
 * on each change, and pushes the level to the driver.
 */
void light_brightness_step(light_service **self)
{
    light_service *s = *self;
    unsigned char b = s->brightness;

    if (b < 100) {
        if (s->beam_enabled == 0) {
            if (b == 0) {
                light_drv_set_brightness(s->drv, &s->brightness);
                return;
            }
            /* fall through to ramp down */
        } else {
            s->brightness = (unsigned char)(b + 10);
            light_fade_timer_restart(s->fade_timer);
            s = *self;
            if (s->brightness == 0) {
                light_drv_set_brightness(s->drv, &s->brightness);
                return;
            }
            b = s->brightness;
        }
    }
    if (s->beam_enabled == 0) {
        s->brightness = (unsigned char)(b - 10);
        light_fade_timer_restart((*self)->fade_timer);
        light_drv_set_brightness((*self)->drv, &(*self)->brightness);
        return;
    }
    light_drv_set_brightness(s->drv, &s->brightness);
}

/* ---- light_set_mode @0x15dc20 --------------------------------------------
 * Applies a requested light mode: 1=on, 2=off, 0=all-off, anything else=auto
 * (re-evaluate against the last averaged lux sample).
 */
void light_set_mode(light_service *self, int mode, char force)
{
    self->mode = mode;
    if (mode == 1) {
        light_headlight_on(self, force);
        return;
    }
    if (mode == 2) {
        light_headlight_off(self, force);
        return;
    }
    if (mode != 0) {
        light_on_light_sample(self, *(unsigned short *)((char *)self + 0x1cc), 0);
        return;
    }
    if (force == 0 && self->headlight_state == 0)
        return;
    light_apply_beam(self, 0, 0);
    self->headlight_state = 0;
}

/* ---- light_restore @0x15dd40 ---------------------------------------------
 * Re-arms the light subsystem after a state restore: marks active and re-applies
 * the persisted mode.
 */
void light_restore(light_service *self)
{
    common_logf("devices/main/ux/src/light.cpp", 0xe5, LOG_WARN, "restore light");
    self->active = 1;
    light_set_mode(self, self->mode, (char)(self->headlight_state == 3));
}

/* ---- light_on_mode_msg @0x15dc70 -----------------------------------------
 * "mode" setting handler: accepts an integral/float variant in [0,3] and
 * applies it via light_set_mode(force=1). Out-of-range values are rejected.
 */
void light_on_mode_msg(light_service **self, const char *variant)
{
    char tag = *variant;
    int ok;

    if ((unsigned char)(tag - 5) < 2) {
        long v = *(long *)(variant + 8);
        ok = (v >= 0) && (v <= 3);
    } else if (tag == 7) {
        double v = *(double *)(variant + 8);
        ok = (v >= 0.0) && (v <= 3.0);
    } else {
        ok = 0;
    }
    if (ok) {
        int mode = light_json_to_int(variant);
        light_set_mode(*self, mode, 1);
    }
}

/* ---- light_on_dark_threshold_msg @0x15b9f0 -------------------------------
 * "dark_threshold" setting handler: accepts an integral/float variant in
 * [0,0xffff] and stores it at +0x1ce.
 */
void light_on_dark_threshold_msg(light_service **self, const char *variant)
{
    char tag = *variant;
    int ok = 0;

    if ((unsigned char)(tag - 5) < 2) {
        long v = *(long *)(variant + 8);
        ok = (v >= 0) && (v <= 0xffff);
    } else if (tag == 7) {
        double v = *(double *)(variant + 8);
        ok = (v >= 0.0) && (v <= 65535.0);
    }
    if (ok)
        (*self)->dark_threshold = light_json_to_u16(variant);
}

/* ---- light_on_light_threshold_msg @0x15bae0 ------------------------------
 * "light_threshold" setting handler: accepts an integral/float variant in
 * [0,0xffff] and stores it at +0x1d0.
 */
void light_on_light_threshold_msg(light_service **self, const char *variant)
{
    char tag = *variant;
    int ok = 0;

    if ((unsigned char)(tag - 5) < 2) {
        long v = *(long *)(variant + 8);
        ok = (v >= 0) && (v <= 0xffff);
    } else if (tag == 7) {
        double v = *(double *)(variant + 8);
        ok = (v >= 0.0) && (v <= 65535.0);
    }
    if (ok)
        (*self)->light_threshold = light_json_to_u16(variant);
}
