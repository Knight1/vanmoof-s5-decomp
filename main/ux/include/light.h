/*
 * light.h — LightService (reconstructed). UXService light/auto-headlight
 * manager for the i.MX8 `ux` service. OEM: devices/main/ux/src/light.cpp.
 *
 * Object offsets (program "ux", base 0x100000):
 *   +0x08    instance mutex (light-state listeners)
 *   +0x38    listener list head
 *   +0xd0    light driver handle (frontlight/rearlight LED controller)
 *   +0xf0    mode (0=off 1=on 2=off-explicit 3=auto)
 *   +0xf8    fade timer (50ms periodic, runs light_brightness_step)
 *   +0x1a8   brightness (0..100, stepped ±10)
 *   +0x1b0   lux ring-buffer base ptr
 *   +0x1b8   lux ring-buffer end ptr
 *   +0x1c8   ring write index
 *   +0x1ca   ring fill count
 *   +0x1cc   rolling average lux
 *   +0x1d2   beam high/low flag
 *   +0x1d3   beam-enabled flag
 *   +0x1d4   restored/active flag
 *   +0x1d5   is_dark flag (hysteresis latch)
 *   +0x1d8   headlight state (0=none 1=on 2=off 3=...)
 *   +0x1ce   dark threshold (default 0x32 == 50)
 *   +0x1d0   light threshold (default 0x8c == 140)
 */
#ifndef LIGHT_H
#define LIGHT_H

#include "ux_common.h"

typedef struct light_service light_service;

void light_ctor(light_service *self, void *vm, void *storage);          /* 0x15bbd0 */
void light_on_light_sample(light_service *self, unsigned short lux, char force); /* 0x15d820 */
void light_headlight_on (light_service *self, char force);             /* 0x15d650 */
void light_headlight_off(light_service *self, char force);             /* 0x15d770 */
void light_apply_beam(light_service *self, unsigned char enabled, char high); /* 0x15d420 */
void light_brightness_step(light_service **self);                      /* 0x15d370 */
void light_set_mode(light_service *self, int mode, char force);        /* 0x15dc20 */
void light_restore(light_service *self);                               /* 0x15dd40 */

void light_on_mode_msg           (light_service **self, const char *variant); /* 0x15dc70 */
void light_on_dark_threshold_msg (light_service **self, const char *variant); /* 0x15b9f0 */
void light_on_light_threshold_msg(light_service **self, const char *variant); /* 0x15bae0 */

/* ---- vendor framework helpers modelled at the call site ------------------ */
/* VM-call registration (FUN_0015f170/0014eec0/...): name + handler. Returns
 * nonzero on failure (the ctor throws std::runtime_error on it). */
extern int light_vm_register(void *vm, const char *name, void *handler);
extern void light_storage_register_default(void *storage, const char *key, int idx,
                                           void *ctx,  ux_fn cb);
/* light driver (LED controller @ +0xd0) primitives. */
extern void light_drv_set_front(void *drv, unsigned char brightness);  /* FUN_0017c650 */
extern void light_drv_set_rear (void *drv, unsigned char brightness);  /* FUN_0017b2d0 */
extern void light_drv_set_brightness(void *drv, unsigned char *level); /* FUN_0017c3c0 */
extern void light_drv_front_beam(void *drv, void *frame);              /* FUN_0017c900 */
extern void light_drv_rear_beam (void *drv, void *frame);              /* FUN_0017b580 */
/* fire the registered light-state std::function listeners. */
extern void light_listeners_fire(light_service *self, void *cb);
/* json variant decode + tag test (FUN_00144b20 / FUN_00144f50). */
extern int  light_json_to_int(const char *variant);
extern unsigned short light_json_to_u16(const char *variant);
/* fade-timer restart (FUN_00186fa0). */
extern void light_fade_timer_restart(void *timer);

#endif /* LIGHT_H */