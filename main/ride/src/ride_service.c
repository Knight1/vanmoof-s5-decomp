/*
 * ride_service.c — reconstructed VanMoof S5 i.MX8 `ride` service: the
 * RideService core (motor-enable gate + assist-level setter). Behaviour C from
 * the decompiled AArch64 (program "ride", base 0x100000).
 *
 * Source path in the OEM image: devices/main/ride/src/ride_service.cpp
 */
#include "ride_common.h"
#include "ride_service.h"

/*
 * Cached last-logged gate state for the !!EnableMotor edge-detect log
 * (DAT_00175358..0017535c — five bytes: PoweredOn, Pedalling, SufficientSOC,
 * Spinning, CommRunning). Module-static, matching the OEM .bss layout.
 */
static char rs_last_powered_on;     /* DAT_00175358 */
static char rs_last_pedalling;      /* DAT_00175359 */
static char rs_last_sufficient_soc; /* DAT_0017535a */
static char rs_last_spinning;       /* DAT_0017535b */
static char rs_last_comm_running;   /* DAT_0017535c */

/*
 * ride_service_ctor — OEM 0x1106e0.
 *
 * RideService(IMotor* motor, IPower* power, SspProtocol* ssp,
 *             RideStrategy* strategy, StateClient* sc, IPedalSensor* pedal)
 *
 * Installs the two vtables (the RideService object and its embedded
 * IMotorEnableSource sub-object), builds the StateClient-backed config field
 * (+2, size 300), zero-inits the sensor/telemetry cache, links the four
 * interface pointers, and finally hands the IMotorEnableSource interface to
 * the strategy (strategy vtable slot +0x20).
 *
 * Param mapping from main(): param_2=imotor, param_3=ipower(ssp side),
 * param_4=strategy, param_5=state-client, param_6=field src, param_7=pedal.
 */
void ride_service_ctor(void **self,
                       void *motor, void *power,
                       istrategy *strategy, void *state_client,
                       void *field_src, void *pedal)
{
    void **p = self;

    /* transient vtables while the StateClient field sub-object is built */
    p[0] = (void *)0x16fa18;
    p[1] = (void *)0x16fad8;
    rs_state_client_ctor(&p[2], field_src);          /* FUN_00129fc0(.., 300) */

    /* final RideService + IMotorEnableSource vtables */
    p[0] = (void *)0x16ef48;
    p[1] = (void *)0x16f020;
    p[2] = (void *)0x16f050;

    p[0x0a] = 0; p[0x0b] = 0; p[0x0c] = 0;
    p[0x0d] = 0; p[0x0e] = 0; p[0x0f] = 0;
    rs_sensor_cache_ctor(&p[0x10]);                  /* FUN_001099b0 */

    /* three empty intrusive lists (head points at itself) */
    p[0x16] = &p[0x18]; p[0x17] = 0; p[0x18] = 0; p[0x19] = 0;
    p[0x1a] = &p[0x1c]; p[0x1b] = 0; p[0x1c] = 0; p[0x1d] = 0;
    p[0x1e] = &p[0x20]; p[0x1f] = 0; p[0x20] = 0; p[0x21] = 0;

    p[0x22] = strategy;          /* RideStrategy */
    p[0x23] = motor;             /* IMotor */
    p[0x24] = power;             /* IPower  */
    p[0x25] = state_client;      /* StateClient */
    rs_motorenable_subobj_ctor(&p[0x26]);            /* FUN_00116050 */

    p[0x2b] = 0; p[0x2c] = 0;
    p[0x29] = pedal;             /* IPedalSensor */
    *(uint16_t *)&p[0x2a]                 = 0;
    *(uint16_t *)((char *)p + 0x156)      = 0;

    /* second empty intrusive list */
    p[0x2e] = &p[0x2d];
    p[0x2d] = &p[0x2d];
    p[0x2f] = 0;

    /* strategy->AttachMotorEnableSource(&this->motorenable_iface) — vt+0x20 */
    istrategy_attach_motorenable(strategy, &p[1]);
}

/*
 * ride_service_should_enable_motor — OEM 0x11bb00.
 *
 * EnableMotor = PoweredOn && Pedalling && SufficientSOC ? Spinning : 0
 *   PoweredOn      = motor->IsPoweredOn()        (IMotor   vt+0x28)
 *   Pedalling      = power->IsPedalling()        (IPower   vt+0x10)
 *   SufficientSOC  = pedal->GetSufficientSOC()   (IPedalSensor vt+0x28)
 *   Spinning       = power->IsSpinning()         (IPower   vt+0x20)
 *   CommRunning    = pedal->IsCommRunning()      (IPedalSensor vt+0x30)
 *
 * On any change in the five gate inputs vs. the cached values, re-reads them
 * and emits the WARN "!!EnableMotor" status line (ride_service.cpp:0x115).
 */
uint8_t ride_service_should_enable_motor(ride_service *self)
{
    uint8_t enable;

    if (!imotor_is_powered_on(self->motor) ||
        !ipower_is_pedalling(self->power) ||
        !ipedal_is_spinning(self->pedal)) {
        /* NOTE: pedal+0x28 in the OEM is the SufficientSOC gate; see below. */
        enable = 0;
    } else {
        enable = (uint8_t)ipower_sufficient_soc(self->power);
    }

    /* edge-detect: only log when one of the five inputs changed */
    if (rs_last_powered_on     != (char)imotor_is_powered_on(self->motor) ||
        rs_last_pedalling      != (char)ipower_is_pedalling(self->power)  ||
        rs_last_sufficient_soc != (char)ipedal_is_spinning(self->pedal)   ||
        rs_last_spinning       != (char)ipower_sufficient_soc(self->power)||
        rs_last_comm_running   != (char)ipedal_comm_running(self->pedal)) {

        char powered_on     = (char)imotor_is_powered_on(self->motor);
        char pedalling      = (char)ipower_is_pedalling(self->power);
        char sufficient_soc = (char)ipedal_is_spinning(self->pedal);
        char spinning       = (char)ipower_sufficient_soc(self->power);
        char comm_running   = (char)ipedal_comm_running(self->pedal);

        rs_last_powered_on     = powered_on;
        rs_last_pedalling      = pedalling;
        rs_last_sufficient_soc = sufficient_soc;
        rs_last_spinning       = spinning;
        rs_last_comm_running   = comm_running;

        common_logf("devices/main/ride/src/ride_service.cpp", 0x115, LOG_WARN,
                    "!!EnableMotor: %d, Pedalling: %d, PoweredOn: %d, "
                    "Spinning: %d, SufficientSOC: %d, Motor comm running: %d",
                    enable, powered_on, pedalling,
                    sufficient_soc, spinning, comm_running);
    }

    return enable;
}

/*
 * ride_service_set_assist_level — OEM 0x11bd90.
 *
 * Validates 0..4 (else WARN "Error, invalid assist level" :0x121), then under
 * the comm mutex forwards the level to the strategy (RideStrategy vt+0x10) and
 * logs "Assist level:  %d." (:0x127, note the OEM's double space).
 */
void ride_service_set_assist_level(ride_service *self, uint8_t level)
{
    if (level > 4) {
        common_logf("devices/main/ride/src/ride_service.cpp", 0x121, LOG_WARN,
                    "Error, invalid assist level, should be 0-4: %d.", level);
        return;
    }

    rd_lock(&self->comm_mutex);
    istrategy_set_assist_level(*(istrategy **)((char *)self + 0xb0), level);
    common_logf("devices/main/ride/src/ride_service.cpp", 0x127, LOG_WARN,
                "Assist level:  %d.", level);
    rd_unlock(&self->comm_mutex);
}
