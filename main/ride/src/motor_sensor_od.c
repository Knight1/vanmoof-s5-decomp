/*
 * motor_sensor_od.c — reconstructed VanMoof S5 i.MX8 `ride` motor-sensor OD
 * client. Program "ride" (AArch64, base 0x100000). Binary source path:
 * devices/main/ride/src/motor_sensor_od.cpp.
 *
 * Receives CANopen reports carrying motor speed, pedal torque and three status
 * bits, plus a bike-model string that selects the motor-config record. STL
 * dtors/vtable thunks (motor_sensor_od_dtor / _dtor_delete) are vendor glue and
 * are not reconstructed.
 */
#include "ride_common.h"
#include "motor_sensor_od.h"

#define MSOD_SRC "devices/main/ride/src/motor_sensor_od.cpp"

/* OEM strings: A5 model label @0x14b5a8, S5 model label @0x14b5b0. */
static const char MODEL_A5[] = "A5";
static const char MODEL_S5[] = "S5";

/* OEM 0x113c70 — bike-model OD callback. Logs motor_sensor_od.cpp:0x23 (WARN)
 * with the model label, then selects the motor-config record by the first
 * character of the model string: 'A'/'V' -> cfg[0] (A5/V), 'S'/'L' -> cfg[1]
 * (S5/L). On first selection it also latches cfg.rest into +0x84 and sets the
 * initialised flag (+0x88); on later calls it copies the whole 8-byte record. */
int motor_sensor_od_model_callback(motor_sensor_od *self, void *unused, const char *model)
{
    (void)unused;
    char c = model[0];
    const char *label = (c == 'S' || c == 'L') ? MODEL_S5 : MODEL_A5;

    common_logf(MSOD_SRC, 0x23, LOG_WARN,
                "Motor sensor callback - Bike model : %s", label);

    c = model[0];
    if (c == 'A' || c == 'V') {
        if (!self->model_initd) {
            self->model_byte  = g_motor_cfg[0].byte0;
            self->model_cfg   = g_motor_cfg[0].rest;
            self->model_initd = 1;
            return 0;
        }
        /* re-copy 8-byte record (byte0 + rest) from cfg[0] */
        self->model_byte = g_motor_cfg[0].byte0;
        self->model_cfg  = g_motor_cfg[0].rest;
    } else if (c == 'S' || c == 'L') {
        if (self->model_initd) {
            self->model_byte = g_motor_cfg[1].byte0;
            self->model_cfg  = g_motor_cfg[1].rest;
            return 0;
        }
        self->model_byte  = g_motor_cfg[1].byte0;
        self->model_cfg   = g_motor_cfg[1].rest;
        self->model_initd = 1;
    }
    return 0;
}

/* OEM 0x113da0 — sensor-report parse.
 *   speed  (+0x68) = r->speed (i16, buf[2..3])
 *   bit2/1/0 (+0x72/0x73/0x74) = buf[0] bits 2/1/0
 *   pedal  (+0x70) = (i8)buf[1] sign-extended
 * If speed, pedal and all three status bits are 0, warn
 * motor_sensor_od.cpp:0x50 "motor-sensor sensor data are all 0".
 * Then forward buf[4]*10 + 0x14 to the publish helper (FUN_0012a190). */
int motor_sensor_od_handle_report(motor_sensor_od *self, void *unused, const motor_sensor_report *r)
{
    (void)unused;
    uint8_t status = r->status;
    int16_t speed  = r->speed;

    self->speed       = speed;
    self->status_bit2 = (status >> 2) & 1;
    self->status_bit1 = (status >> 1) & 1;
    self->status_bit0 = status & 1;

    int8_t pedal = r->pedal;
    self->pedal  = pedal;

    if (speed == 0 && pedal == 0 &&
        (status & 1) == 0 && ((status >> 1) & 1) == 0 && ((status >> 2) & 1) == 0) {
        common_logf(MSOD_SRC, 0x50, LOG_DEBUG,
                    "motor-sensor sensor data are all 0");
    }

    /* publish at +8 with period = buf[4]*10 + 0x14 (ms) */
    msod_publish(&self->pub_obj[8], (unsigned)r->period * 10 + 0x14);
    return 0;
}

/* OEM 0x113b00 — reset speed/pedal/status_word low and status bit0. */
void motor_sensor_od_reset(motor_sensor_od *self)
{
    self->speed = 0;
    self->pedal = 0;          /* clears the u32 at +0x70 (pedal + bits 2/1) */
    self->status_bit2 = 0;
    self->status_bit1 = 0;
    self->status_bit0 = 0;    /* (+0x74) */
}

/* OEM 0x113b20 */
int16_t motor_sensor_od_get_speed(const motor_sensor_od *self)       { return self->speed; }
/* OEM 0x113b30 */
int16_t motor_sensor_od_get_pedal_value(const motor_sensor_od *self) { return self->pedal; }
/* OEM 0x113b40 */
uint8_t motor_sensor_od_get_model_byte(const motor_sensor_od *self)  { return self->model_byte; }
/* OEM 0x113b50 */
uint32_t motor_sensor_od_get_status_word(const motor_sensor_od *self){ return self->status_word; }
