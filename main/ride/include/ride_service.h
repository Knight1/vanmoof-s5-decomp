/*
 * ride_service.h — module-private declarations for the reconstructed VanMoof
 * S5 i.MX8 `ride` service core (RideService). Included after ride_common.h.
 *
 * RideService owns the IMotor / IPower / IPedalSensor / RideStrategy handles
 * and the SSP protocol link; it decides whether the motor may be enabled and
 * applies the rider's assist level. Program "ride", AArch64, base 0x100000.
 */
#ifndef RIDE_SERVICE_H
#define RIDE_SERVICE_H

#include "ride_common.h"

/*
 * RideService instance layout (byte offsets quoted from the OEM image).
 * Only the fields the core logic touches are modelled by name; the rest of the
 * 0x180-byte object (CANopen sensor cache, comm-state mutex at +0x60, SSP send
 * queue at +0x110/+0x118/+0x120 telemetry slots) is opaque framework state.
 */
typedef struct ride_service {
    void       *vptr;               /* +0x00 RideService vtable (0016ef48) */
    void       *vptr_motorenable;   /* +0x08 IMotorEnableSource (0016f020) */
    uint8_t     opaque_10[0x60 - 0x10];
    rd_mutex    comm_mutex;         /* +0x60 guards assist/comm state */
    uint8_t     opaque_88[0x98 - 0x88];
    ipedal     *pedal;             /* +0x98 IPedalSensor */
    ipower     *power;             /* +0xa0 IPower */
    imotor     *motor;             /* +0xa8 IMotor */
    uint8_t     opaque_b0[0x180 - 0xb0];
} ride_service;

/* IMotor / IPower / IPedalSensor vtable-slot dispatch helpers ------------- */
/* IMotor  : +0x28 = IsPoweredOn() */
extern int  imotor_is_powered_on(imotor *m);
/* IPower  : +0x10 = IsPedalling(); +0x20 = GetSufficientSOC() */
extern int  ipower_is_pedalling(ipower *p);
extern int  ipower_sufficient_soc(ipower *p);
/* IPedalSensor: +0x28 = IsSpinning(); +0x30 = IsCommRunning() */
extern int  ipedal_is_spinning(ipedal *ps);
extern int  ipedal_comm_running(ipedal *ps);

/* RideStrategy: +0x10 = SetAssistLevel(level) */
extern void istrategy_set_assist_level(istrategy *s, uint8_t level);

/* internal framework glue used by the ctor (modelled) -------------------- */
extern void rs_state_client_ctor(void *field_300, void *state_client);
extern void rs_motorenable_subobj_ctor(void *sub);
extern void rs_sensor_cache_ctor(void *cache);
extern void istrategy_attach_motorenable(istrategy *s, void *motorenable_iface);

/* RideService.set_assist_level (0x11bd90) — mutex-guarded setter. */
void ride_service_set_assist_level(ride_service *self, uint8_t level);

#endif /* RIDE_SERVICE_H */
