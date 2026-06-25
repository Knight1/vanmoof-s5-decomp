/*
 * motor.h — module-private declarations for the reconstructed VanMoof S5
 * i.MX8 `ride` motor controller glue (program "ride", base 0x100000).
 *
 * Included AFTER ride_common.h. Models the C++ Motor class, its IMotor /
 * IPower / IPedalSensor vtable calls, the SSP frame builder, and the
 * std::list<SpeedSample> brake-light window as opaque/private helpers.
 */
#ifndef RIDE_MOTOR_H
#define RIDE_MOTOR_H

#include "ride_common.h"
#include <math.h>

/*
 * Motor service object (this == param_1). Only the fields touched by the
 * reconstructed functions are modelled; the rest of the C++ object is opaque
 * padding so the OEM byte offsets line up (offsets quoted from motor.cpp).
 */
typedef struct motor_service {
    uint8_t  _pad0[0x110];
    ssp_proto *ssp;          /* +0x110 : SspProtocol* (vtbl +0x10 = send frame) */
    imotor   *motor;         /* +0x118 : IMotor*  (+0x18 wheel-speed, +0x20 rpm, +0x28 type) */
    ipower   *power;         /* +0x120 : IPower*  (+0x10 current, +0x18 v, +0x20 temp) */
    uint8_t  _pad128[0x20];
    istrategy *strategy;     /* +0x148 : RideStrategy* (+0x20 = set OD value) */
    uint8_t  _pad150[0x14];
    /* +0x150 : motor status / gate-drv status registers (read from CAN) */
    uint16_t status;         /* +0x150 */
    uint16_t gate_drv1;      /* +0x152 */
    uint16_t gate_drv2;      /* +0x154 */
    uint8_t  _pad156[2];
    uint16_t current_raw;    /* +0x158 */
    uint8_t  _pad15a[2];
    uint16_t drv_temp;       /* +0x15c */
    uint8_t  _pad15e[6];
    uint16_t pub_value;      /* +0x164 : last value published over SSP op 0x19 */
    uint8_t  ver_present;    /* +0x167 : motor-controller-version-known flag */
    /* +0x168..+0x178 : std::list<speed_sample> ring (head/tail/size) */
    void    *bl_list_head;   /* +0x168 */
    void    *bl_list_tail;   /* +0x170 */
    uint64_t bl_list_size;   /* +0x178 */
    rd_mutex ver_mutex;      /* +0x50  : version-string guard (modelled) */
    rd_cond  ver_cond;       /* +0x80  : version-ready cond-var (modelled) */
    char    *fw_version;     /* +0xb0  : std::string controller fw version */
    char    *hw_version;     /* +0xd0  : std::string controller hw version */
    char    *serial;         /* +0xf0  : std::string controller serial */
} motor_service;

/* Motor controller version triple returned by motor_get_version. */
typedef struct motor_version {
    char *fw;
    char *hw;
    char *serial;
} motor_version;

/* ---- SSP frame builder (modelled; real impl lives in ssp module) -------- */
extern void ssp_frame_begin(void *frame, int opcode, int type);
extern void ssp_frame_begin3(void *frame, int opcode, int seq, int type);
extern void ssp_frame_append(void *frame, unsigned long value);
extern void ssp_frame_free(void *frame);

/* ---- IMotor / IPower / IPedalSensor / RideStrategy vtable shims --------- */
extern void     ssp_send(ssp_proto *p, void *frame);            /* vtbl +0x10 */
extern uint16_t imotor_wheel_speed(imotor *m);                  /* vtbl +0x18 */
extern int16_t  imotor_rpm(imotor *m);                          /* vtbl +0x20 */
extern int16_t  imotor_type(imotor *m);                         /* vtbl +0x28 */
extern int      ipower_current(ipower *p);                      /* vtbl +0x10 */
extern uint8_t  ipower_voltage(ipower *p);                      /* vtbl +0x18 */
extern int16_t  ipower_temp(ipower *p);                         /* vtbl +0x20 */
extern void     strategy_set_value(istrategy *s, const char *key,
                                   int value);                  /* vtbl +0x20 */

/* ---- std::list<speed_sample> brake-light window (modelled) -------------- */
typedef struct speed_sample speed_sample;
extern void speed_ratio_window_push(motor_service *self, uint16_t speed);
extern long speed_ratio_window_sum(const motor_service *self); /* sum of speeds */
extern void speed_ratio_window_clear(motor_service *self);     /* drop & restore */
extern void speed_ratio_window_pop_front(motor_service *self); /* drop oldest */

/* version wait helpers (cond-var modelled) */
extern bool motor_version_known(motor_service *self);
extern bool motor_version_wait(motor_service *self, long timeout_ns);

void     motor_handle_request(motor_service *self, char opcode, void *frame);
void     motor_publish_value(motor_service *self, uint16_t value);
void     motor_request_version(motor_service *self);
motor_version *motor_get_version(motor_version *out, motor_service *self);
void     motor_assist_filter(float cadence, float torque, motor_service *self,
                             char reset);
float    motor_current_smooth(motor_service *self, char reset);
void     motor_log_status(motor_service *self, char force,
                          int16_t wh_speed, int16_t pdl_speed, int16_t pdl_cnt,
                          int16_t pdl_torque, int16_t mtr_tmp);
void     motor_update_brake_lights(motor_service *self);
void     ride_service_publish_telemetry(motor_service *self, char active);

#endif /* RIDE_MOTOR_H */
