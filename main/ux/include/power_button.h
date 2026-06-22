/*
 * power_button.h — PowerButtonService (reconstructed). Reads the Linux input
 * event device for the i.MX8 power key and fans out press/release events to
 * registered std::function listeners. OEM: devices/main/ux/src/power_button.cpp.
 *
 * Object offsets (program "ux", base 0x100000):
 *   +0x08    instance mutex (button-event listeners)
 *   +0x38    listener list head
 *   +0x70    multi-press / hold timer object
 *   +0x120   prev button value (edge detection)
 *   +0x124   multi-press counter
 *   +0x130   running flag (reader thread)
 *   +0x134   input-event device fd (+ open marker at +0x138)
 *   +0x12c   open marker / valid flag
 */
#ifndef POWER_BUTTON_H
#define POWER_BUTTON_H

#include "ux_common.h"
#include <stdint.h>

typedef struct power_button_service power_button_service;

/* Linux input_event (24 bytes: timeval(16) + u16 type + u16 code + s32 value). */
typedef struct power_input_event {
    uint64_t tv_sec;
    uint64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
    char     valid;          /* poll set a full event */
} power_input_event;

void power_button_ctor(power_button_service *self, void **dev_path, int variant); /* 0x178090 */
void power_button_dtor(power_button_service *self);                               /* 0x1782d0 */
void power_button_open_device(int *fd_out, void **dev_path);                      /* 0x177c10 */
unsigned power_button_supports_key_events(int *fd, int variant);                  /* 0x177b30 */
void power_button_poll_read_event(power_input_event *out, int *pollfd);           /* 0x177990 */
void power_button_reader_loop(power_button_service **arg);                        /* 0x178370 */

/* ---- vendor framework helpers modelled at the call site ------------------ */
extern void power_button_listeners_fire_down(power_button_service *self); /* FUN_00177850 */
extern void power_button_listeners_fire_up  (power_button_service *self); /* FUN_001778c0 */
/* hold/multi-press timer (object @ +0x70): restart with a timeout in ms. */
extern void power_button_timer_arm(void *timer, unsigned ms);            /* FUN_001870c0 */
extern char power_button_timer_active(void *timer);                      /* FUN_00186700 */

#endif /* POWER_BUTTON_H */