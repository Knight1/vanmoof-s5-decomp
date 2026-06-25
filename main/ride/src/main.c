/*
 * main.c — reconstructed VanMoof S5 i.MX8 `ride` service entry point and
 * RideApp wiring. Behaviour C from the decompiled AArch64 (program "ride",
 * base 0x100000). Source path in the OEM image: devices/main/ride/src/main.cpp
 *
 * STL container/option-parser glue (getopt option vector, std::string moves,
 * the std::function recyclers `*_cb_manager`) is modelled at the call site;
 * the load-bearing logic is the CLI flags (-i/-s/-t), the object graph the
 * service is built from, and the three RideApp MQTT subscriptions.
 */
#include "ride_common.h"
#include "ride_service.h"
#include "main.h"

/* getopt-parsed CLI state. */
typedef struct ride_cli {
    char     ip[64];     /* -i <ip>  (simulate-from-network) */
    char     sim_active; /* -i present  */
    char     sim_speed;  /* -s <speed kmh> present */
    uint16_t assist_arg; /* -s value (requested speed setting) */
    uint16_t assist_type;/* -t <type>: 0 speed-control, 1 speed-ratio; default 0x19 */
} ride_cli;

extern int  ride_getopt(int argc, char **argv, const char *optstring,
                        const char *usage[], int usage_n, ride_cli *out);
extern void state_client_ctor(void *sc, int argc, char **argv, const char *name);

/*
 * main — OEM 0x10a1f0.
 *
 * 1. Default serial device "/dev/ttymxc3"; CLI optstring "ip:s:t:".
 *    Usage lines: -s <speed kmh> (simulate, SPINS the motor),
 *                 -t <Motor assist type> (0=speed control,1=speed ratio,def 0).
 * 2. Build StateClient("ride-service"), SerialTransport(dev,0x1002),
 *    SspProtocol(transport).
 * 3. Pick the RideStrategy by flags:
 *      sim/-i set     -> speed-control strategy (FUN_00121f10, assist_type)
 *      else type==0   -> default RideStrategy   (FUN_00121ea0, level 4)
 *      else type!=0   -> SpeedRatio strategy     (level 4)
 * 4. Build the IMotor/IPower sources (real serial vs. simulated), the
 *    MotorSensorOd, then RideService and RideApp; "Run Ride"; ride_app_run.
 * 5. On return: "Shutting down Ride" -> dtor chain -> "Ride shut down".
 */
int main(int argc, char **argv)
{
    static const char *usage[3] = {
        "-s <speed kmh> - Simulate with requested speed setting, be careful, "
        "this will SPIN the motor.",
        "-t <Motor assist type> - Set the type of motor assistance; "
        "0 = speed control, 1 = speed ratio control, default = 0.",
        "" /* DAT_0014b430 */
    };

    char       dev[64];          /* default "/dev/ttymxc3" */
    ride_cli   cli;
    state_client *sc;
    serial_port  *transport;
    ssp_proto    *ssp;
    istrategy    *strategy;
    void *motor, *power, *pedal_sensor;  /* IMotor / IPower / MotorSensorOd */
    void *ride_service;
    ride_app app;

    serial_strdup(dev, "/dev/ttymxc3");

    cli.sim_active  = 0;
    cli.sim_speed   = 0;
    cli.assist_arg  = 0;
    cli.assist_type = 0x19;
    state_client_partial_init(&sc);

    if (ride_getopt(argc, argv, "ip:s:t:", usage, 3, &cli) != 0) {
        /* getopt error path: throw std::runtime_error (DAT_0010f6b0). */
        ride_throw_runtime_error("ip:s:t:");
    }

    state_client_ctor(&sc, argc, argv, "ride-service");

    transport = serial_transport_new(dev, 0x1002);
    ssp       = ssp_protocol_new(transport);

    /* strategy selection (uses the parsed flags) */
    if (cli.sim_active == 0 && cli.sim_speed == 0) {
        if (cli.assist_arg == 0) {
            strategy = ride_strategy_speed_new(4);            /* FUN_00121ea0 */
        } else {
            strategy = ride_strategy_speed_ratio_new(4);      /* speed_ratio */
        }
    } else {
        strategy = ride_strategy_sim_type_new((uint8_t)cli.assist_type);
    }

    /*
     * IMotor / IPower / pedal sources: real serial-backed (default), or the
     * network/sim variants when -i/-s are set. The OEM threads StateClient
     * accessors (FUN_0012db70/80/90) into each ctor; modelled as sc here.
     */
    if (cli.sim_active != 0) {
        motor = ride_motor_sim_new(sc);
        power = ride_power_sim_new((uint8_t)cli.assist_type);
        pedal_sensor = ride_pedal_stub_new();
    } else if (cli.sim_speed != 0) {
        motor = ride_motor_net_new(sc);
        power = ride_power_net_new(sc);
        pedal_sensor = ride_pedal_stub_new();
    } else {
        motor = ride_motor_serial_new(sc);
        power = ride_power_serial_new(sc);
        pedal_sensor = ride_motor_sensor_od_new(sc);          /* FUN_00116450 */
    }

    {
        void **rs = op_new(0x180);
        ride_service_ctor(rs, power, pedal_sensor, ssp, strategy, sc, sc);  /* OEM: arg4=ssp, arg5=strategy */
        ride_service = rs;
    }

    ride_app_ctor(&app, sc, ride_service, motor, power, /*mqtt*/(mqtt_client *)sc, strategy);

    common_logf("devices/main/ride/src/main.cpp", 0x6e, LOG_WARN, "Run Ride");
    ride_app_run(&app);

    common_logf("devices/main/ride/src/main.cpp", 0x71, LOG_WARN,
                "Shutting down Ride");
    ride_app_dtor(&app);

    istrategy_delete(strategy);
    if (ride_service) op_vdelete(ride_service);
    if (motor)        op_vdelete(motor);
    if (power)        op_vdelete(power);
    if (pedal_sensor) op_vdelete(pedal_sensor);
    if (ssp)          op_vdelete(ssp);
    if (transport)    serial_port_delete(transport);

    common_logf("devices/main/ride/src/main.cpp", 0x78, LOG_WARN,
                "Ride shut down");
    return 0;
}

/*
 * ride_app_ctor — OEM 0x11b200.
 *
 * RideApp(StateClient* sc, RideService* svc, IMotor* motor, IPower* power,
 *         IMQTTClient* mqtt, RideStrategy* strategy)
 *
 * Chains the base ServiceApp ctor, installs the RideApp vtable (0016fe90),
 * caches the six handles, builds the motor-enable config field, and registers
 * three OD/MQTT change callbacks against the strategy:
 *   motorenable -> ride_service_update_motor_enable (key DAT_0014c600)
 *   status      -> LAB_001200d0                      (key DAT_0014c608)
 *   version     -> LAB_0011d7d0                      (key DAT_0014c610)
 */
void ride_app_ctor(ride_app *self, state_client *sc, void *ride_service,
                   void *motor, void *power, mqtt_client *mqtt,
                   istrategy *strategy)
{
    void **p = (void **)self;
    void  *menable_field;
    void  *cb;

    ride_app_base_ctor(self, sc, mqtt, 0);            /* FUN_00130850 */
    p[0] = (void *)0x16fe90;                          /* RideApp vtable */

    p[0x0c] = 0; p[0x0d] = 0; p[0x0e] = 0; p[0x0f] = 0;
    *(uint32_t *)&p[0x0e] = 1;
    p[0x10] = 0; p[0x11] = 0;
    p[0x12] = sc;
    p[0x13] = ride_service;
    p[0x14] = motor;
    p[0x15] = power;
    p[0x16] = strategy;
    p[0x17] = 0; p[0x18] = 0; p[0x19] = 0;
    ride_app_field_set_ctor(&p[0x1a]);               /* FUN_001099b0 */
    p[0x20] = 0;

    menable_field = ride_app_motorenable_field_new(); /* op_new(0x40), 0016f350 */
    p[0x20] = (char *)menable_field + 0x10;
    p[0x21] = menable_field;
    *(uint8_t  *)&p[0x22]            = 0;
    *(uint32_t *)((char *)p + 0x114) = 0;

    /* strategy->RegisterMotorEnable(cb=update_motor_enable, key) — vt+0x18 */
    cb = 0;
    mqtt_od_register_via_strategy(strategy, &cb, self,
                                  (ride_fn)ride_service_update_motor_enable,

                                  (ride_fn)ride_app_motorenable_cb_manager,

                                  (const char *)0x14c600);
    ride_app_store_cb(&p[0x17], cb);

    /* strategy->RegisterStatus(cb=LAB_001200d0, key) — vt+0x18 */
    cb = 0;
    mqtt_od_register_via_strategy(strategy, &cb, self,
                                  (ride_fn)ride_app_status_handler,

                                  (ride_fn)ride_app_status_cb_manager,

                                  (const char *)0x14c608);
    ride_app_store_cb(&p[0x18], cb);

    /* strategy->RegisterVersion(cb=LAB_0011d7d0, key) — vt+0x10 */
    cb = 0;
    mqtt_od_register_via_strategy(strategy, &cb, self,
                                  (ride_fn)ride_app_version_handler,

                                  (ride_fn)ride_app_version_cb_manager,

                                  (const char *)0x14c610);
    ride_app_store_cb(&p[0x19], cb);
}

/*
 * ride_app_run — OEM 0x1200e0.
 *
 * Under the subscribe mutex (+0x60), subscribes the three command/settings
 * MQTT topics on the IMQTTClient (+0x90, subscribe = vtable +0x10):
 *   "settings/assist_level" -> ride_app_on_assist_level_msg
 *   "settings/region"       -> ride_app_on_region_msg
 *   "ride/boost"            -> ride_app_on_boost_msg
 */
void ride_app_run(ride_app *self)
{
    rd_lock(&self->sub_mutex);

    mqtt_subscribe(self->mqtt, "settings/assist_level",
                   (mqtt_handler)(void(*)(void))ride_app_on_assist_level_msg, self);
    mqtt_subscribe(self->mqtt, "settings/region",
                   (mqtt_handler)(void(*)(void))ride_app_on_region_msg, self);
    mqtt_subscribe(self->mqtt, "ride/boost",
                   (mqtt_handler)(void(*)(void))ride_app_on_boost_msg, self);

    rd_unlock(&self->sub_mutex);
}

/*
 * ride_app_on_assist_level_msg — OEM 0x11d7e0.
 *
 * MQTT handler for "settings/assist_level": deserialises a single uint8 from
 * the payload (FUN_001214c0) and forwards it to RideService::SetAssistLevel,
 * which validates 0..4 and logs "Assist level:  %d.".
 */
void ride_app_on_assist_level_msg_impl(ride_app *self, const void *payload)
{
    uint8_t       level = 0;
    ride_service *svc   = *(ride_service **)self;   /* svc cached at +0x00 ctx */

    ssp_payload_read_u8(payload, &level);           /* FUN_001214c0 */
    ride_service_set_assist_level(svc, level);
}
