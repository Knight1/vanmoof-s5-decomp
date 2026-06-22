/*
 * state_maintenance.c -- VanMoof S5 i.MX8 `ux` service: MaintenanceStrategy.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000):
 *   state_maintenance_strategy_ctor       0x136a10
 *   state_maintenance_strategy_on_button  0x136890
 *
 * On entering the Maintenance state the strategy announces the
 * battery-locked sound and arms a power-button listener; a button press
 * announces the battery-unlock sound and, on completion, requests a
 * transition back to Standby (OEM tail-call into the state-request thunk
 * 0x13e2a0, which posts state id 2 = UX_STANDBY).
 */
#include "ux_common.h"
#include "state_maintenance.h"

/*
 * Sound-publish completion callback (OEM bound functor at LAB_00136880).
 * The unlock sound carries this completion; it pulls the UXService out of the
 * strategy and posts a ToStandby request (0x13e2a0 -> state 2).
 */
static void state_maintenance_leave_to_standby(void *ctx)
{
    state_maintenance_strategy *self = (state_maintenance_strategy *)ctx;
    ux_to_standby(self->svc);
}

/*
 * state_maintenance_strategy_on_button -- 0x136890
 * Power button pressed while in Maintenance.
 */
void state_maintenance_strategy_on_button(state_maintenance_strategy *self)
{
    common_logf("devices/main/ux/src/state_maintenance_strategy.cpp", 0x11, LOG_WARN,
                "Button press detected - leaving Maintenance state and going to Standby");

    /* Announce the unlock sound; completion -> ToStandby (LAB_00136880). */
    ux_sound_play(self->svc, "sec_battery_unlock");
    state_maintenance_leave_to_standby(self);
}

/* Adapter so the listener can call on_button(void *ctx). */
static void state_maintenance_button_thunk(void *ctx)
{
    state_maintenance_strategy_on_button((state_maintenance_strategy *)ctx);
}

/*
 * state_maintenance_strategy_ctor -- 0x136a10
 * obj size 0x98, vtable &DAT_001fa150.
 */
void state_maintenance_strategy_ctor(state_maintenance_strategy *self,
                                     int state, ux_service *svc)
{
    self->vtable = (const void *)0;   /* &DAT_001fa150 (set by OEM) */
    self->state  = state;
    self->svc    = svc;

    /* Construct the button-press listener over the bike manager (db_60). */
    elock_button_listener_init(&self->button, ux_lock(svc));

    /* Announce the battery-locked sound on entry. */
    ux_sound_play(ux_mqtt(svc) ? svc : svc, "sec_battery_locked");

    /* Arm the power-button listener -> on_button. */
    elock_button_listener_on_press(&self->button,
                                   state_maintenance_button_thunk, self);
}
