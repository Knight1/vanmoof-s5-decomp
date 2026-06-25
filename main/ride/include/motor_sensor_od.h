/*
 * motor_sensor_od.h — module-local model for the VanMoof S5 i.MX8 `ride`
 * service motor-sensor object-dictionary client (program "ride", base
 * 0x100000). Included AFTER ride_common.h. Models
 * devices/main/ride/src/motor_sensor_od.cpp: it tracks motor speed, pedal
 * torque, sensor status bits and the bike-model byte from CANopen OD reports.
 *
 * Object field offsets (verified against the OEM):
 *   +0x68  int16   speed
 *   +0x6c  uint32  status_word (aggregate read by get_status_word)
 *   +0x70  int16   pedal value
 *   +0x72  uint8   status bit 2 (buf[0]>>2 & 1)
 *   +0x73  uint8   status bit 1 (buf[0]>>1 & 1)
 *   +0x74  uint8   status bit 0 (buf[0]    & 1)
 *   +0x80  uint8   model byte (selected motor-config field)
 *   +0x84  uint32  model-config remainder
 *   +0x88  uint8   model-config initialised flag
 */
#ifndef RIDE_MOTOR_SENSOR_OD_H
#define RIDE_MOTOR_SENSOR_OD_H

#include "ride_common.h"

typedef struct motor_sensor_od {
    uint8_t  pub_obj[0x60];   /* +0x00 base / publish glue */
    int16_t  speed;           /* +0x68 */
    uint16_t pad_6a;          /* +0x6a */
    uint32_t status_word;     /* +0x6c (overlaps +0x70/0x72/0x73/0x74) */
    int16_t  pedal;           /* +0x70 */
    uint8_t  status_bit2;     /* +0x72 */
    uint8_t  status_bit1;     /* +0x73 */
    uint8_t  status_bit0;     /* +0x74 */
    uint8_t  pad_75[0x0b];    /* +0x75 */
    uint8_t  model_byte;      /* +0x80 */
    uint8_t  pad_81[3];       /* +0x81 */
    uint32_t model_cfg;       /* +0x84 */
    uint8_t  model_initd;     /* +0x88 */
} motor_sensor_od;

/* CANopen sensor-report frame: buf[0]=status, buf[1]=i8 pedal, buf[2..3]=i16
 * speed, buf[4]=publish-period selector. */
typedef struct motor_sensor_report {
    uint8_t status;
    int8_t  pedal;
    int16_t speed;
    uint8_t period;
} motor_sensor_report;

void     motor_sensor_od_reset(motor_sensor_od *self);
int16_t  motor_sensor_od_get_speed(const motor_sensor_od *self);
int16_t  motor_sensor_od_get_pedal_value(const motor_sensor_od *self);
uint8_t  motor_sensor_od_get_model_byte(const motor_sensor_od *self);
uint32_t motor_sensor_od_get_status_word(const motor_sensor_od *self);
int      motor_sensor_od_model_callback(motor_sensor_od *self, void *unused, const char *model);
int      motor_sensor_od_handle_report(motor_sensor_od *self, void *unused, const motor_sensor_report *r);

/* Global motor-config table (OEM DAT_00175098): A5/V config @[0], S5/L @[1]. */
typedef struct motor_cfg { uint8_t byte0; uint32_t rest; } motor_cfg;
extern const motor_cfg *g_motor_cfg;  /* DAT_00175098 */

/* Publish helper (OEM FUN_0012a190): schedule a sensor publish at `period_ms`. */
void msod_publish(void *pub, unsigned period_ms);

#endif /* RIDE_MOTOR_SENSOR_OD_H */
