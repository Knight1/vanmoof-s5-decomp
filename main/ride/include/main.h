/*
 * main.h — module-private declarations for the reconstructed VanMoof S5 i.MX8
 * `ride` service entry point (main.cpp) and the RideApp object. Included after
 * ride_common.h. Program "ride", AArch64, base 0x100000.
 */
#ifndef RIDE_MAIN_H
#define RIDE_MAIN_H

#include "ride_common.h"
#include "ride_service.h"

/* StateClient handle (common::StateClient) — opaque framework. */
typedef struct state_client state_client;

/*
 * RideApp instance layout (offsets in 8-byte words, from the OEM 0x11b200
 * ctor). Models only what main/app_run touch by name; the rest is opaque.
 */
typedef struct ride_app {
    void        *vptr;              /* +0x00 RideApp vtable (0016fe90) */
    uint8_t      opaque_08[0x60 - 0x08];
    rd_mutex     sub_mutex;        /* +0x60 guards MQTT subscribe set */
    uint8_t      opaque_88[0x90 - 0x88];
    mqtt_client *mqtt;            /* +0x90 IMQTTClient */
    uint8_t      opaque_98[0x180 - 0x98];
} ride_app;

/* main.cpp object graph (constructors reconstructed in sibling modules) --- */
extern serial_port *serial_transport_new(const char *dev, int flags);      /* 0x1097e0 + serial_transport_ctor */
extern void         serial_port_delete(serial_port *sp);
extern ssp_proto   *ssp_protocol_new(serial_port *transport);              /* ssp_protocol_ctor */
extern istrategy   *ride_strategy_speed_new(uint8_t assist_level);          /* FUN_00121ea0 */
extern istrategy   *ride_strategy_speed_ratio_new(uint8_t assist_level);    /* speed_ratio_ride_strategy_ctor */
extern istrategy   *ride_strategy_sim_type_new(uint8_t assist_level);       /* FUN_00121f10 */

/* RideService — 0x1106e0. OEM arg order: (self, a, b, ssp, strategy, sc, pedal);
 * param types kept void* since the call passes the SspProtocol + RideStrategy. */
extern void ride_service_ctor(void **self, void *a, void *b,
                              void *ssp, void *strategy,
                              void *state_client, void *pedal);

/* RideApp lifecycle ------------------------------------------------------- */
void ride_app_ctor(ride_app *self, state_client *sc, void *ride_service,
                   void *motor, void *power, mqtt_client *mqtt,
                   istrategy *strategy);
void ride_app_run(ride_app *self);
extern void ride_app_dtor(ride_app *self);

/* RideApp MQTT callback std::function managers + invokers (modelled) ------ */
extern void ride_app_motorenable_cb_manager(void *op, void *fn, int act);
extern void ride_app_status_cb_manager(void *op, void *fn, int act);
extern void ride_app_version_cb_manager(void *op, void *fn, int act);
extern void ride_app_assist_cb_manager(void *op, void *fn, int act);
extern void ride_app_region_cb_manager(void *op, void *fn, int act);
extern void ride_app_boost_cb_manager(void *op, void *fn, int act);
extern void ride_service_update_motor_enable(void *self);
extern void ride_app_on_assist_level_msg(void *self, const void *payload);
extern void ride_app_on_region_msg(void *self, const void *payload);
extern void ride_app_on_boost_msg(void *self, const void *payload);
extern void ride_app_status_handler(void *self, const void *payload);
extern void ride_app_version_handler(void *self, const void *payload);

/* generic type-erased callback for the OD/strategy registration glue. */
typedef void (*ride_fn)(void);
void mqtt_od_register_via_strategy(void *strategy, void *cb_out, void *ctx,
                                   ride_fn handler, ride_fn manager, const char *key);
void ride_app_store_cb(void *slot, void *cb);

/* sub-interface factories (sim / net / serial variants), selected by CLI. */
imotor *ride_motor_sim_new(void *sc);
imotor *ride_motor_net_new(void *sc);
imotor *ride_motor_serial_new(void *sc);
ipedal *ride_motor_sensor_od_new(void *sc);   /* MotorSensorOd, OEM 0x116450 */
ipedal *ride_pedal_stub_new(void);
ipower *ride_power_sim_new(uint8_t assist_type);
ipower *ride_power_net_new(void *sc);
ipower *ride_power_serial_new(void *sc);

/* misc modelled glue used by main. */
void serial_strdup(void *dst, const char *src);
void state_client_partial_init(void *sc);
void ride_throw_runtime_error(const char *what);
void istrategy_delete(istrategy *s);
void op_vdelete(void *p);   /* virtual delete (vtable+0x08) */
void ssp_payload_read_u8(const void *payload, void *out);  /* decodes a u8 from the OD payload into *out */

/* base ctors for RideApp / StateClient member sub-objects (modelled) ------ */
extern void ride_app_base_ctor(ride_app *self, state_client *sc, mqtt_client *mqtt, int flag);
extern void ride_app_field_set_ctor(void *fieldset);
extern void *ride_app_motorenable_field_new(void);
extern void mqtt_od_register(mqtt_client *mqtt, void *out_cb, void *ctx, const char *key);

#endif /* RIDE_MAIN_H */
