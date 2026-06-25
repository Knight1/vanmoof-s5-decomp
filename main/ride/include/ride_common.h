/*
 * ride_common.h — canonical framework model for the reconstructed VanMoof S5
 * i.MX8 `ride` service (pedal-assist controller). Behaviour-oriented C (C++
 * modelled in C), matching main/update + main/power + main/ux + main/tracking.
 *
 * Every reconstructed TU targets THIS model. The framework — common::IMQTTClient,
 * common::StateClient, the CANopen OD registry, std::thread/mutex/cv/deque/
 * vector/function, the IMotor/IPower/IPedalSensor/RideStrategy interfaces — is
 * VENDOR: modelled as opaque handles + extern prototypes, not reconstructed.
 * OEM addresses are quoted in the .c files (program "ride", base 0x100000).
 */
#ifndef RIDE_COMMON_H
#define RIDE_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common::logf (anchor 0x147630): 1=DBG 2=INFO 3=WARN 4=ERR ----------- */
enum log_level { LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4, LOG_ERR = 4 };
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* ---- vendor framework: opaque handles ----------------------------------- */
typedef struct mqtt_client mqtt_client;       /* common::IMQTTClient */
typedef struct od_registry od_registry;       /* CANopen object-dictionary */
typedef struct imotor      imotor;            /* IMotor   (vtable) */
typedef struct ipower      ipower;            /* IPower   (vtable) */
typedef struct ipedal      ipedal;            /* IPedalSensor (vtable) */
typedef struct istrategy   istrategy;         /* RideStrategy (vtable) */
typedef struct ssp_proto   ssp_proto;         /* SspProtocol */
typedef struct serial_port serial_port;       /* common::SerialPort (termios) */

/* ---- MQTT (telemetry publish) ------------------------------------------- */
void mqtt_publish_int(mqtt_client *c, const char *topic, long v, int qos, int retain);
void mqtt_publish_str(mqtt_client *c, const char *topic, const char *v, int qos, int retain);
typedef void (*mqtt_handler)(void *ctx, const char *topic, const void *payload);
void mqtt_subscribe(mqtt_client *c, const char *topic, mqtt_handler fn, void *ctx);

/* ---- CANopen OD: register a named entry + a change callback -------------- */
typedef void (*od_cb)(void *ctx);
void od_register(od_registry *od, const char *name, od_cb cb, void *ctx);
void od_notify(void *field);   /* FUN_0012a100: field-changed cond-var wake */

/* ---- new/delete + threads/sync (modelled opaque) ------------------------ */
void *op_new(size_t n);
void  op_delete(void *p, size_t n);
typedef struct rd_mutex { void *_o[5]; } rd_mutex;
typedef struct rd_cond  { void *_o[8]; } rd_cond;
void  rd_lock(rd_mutex *m);
void  rd_unlock(rd_mutex *m);
void  rd_notify(rd_cond *c);
void *rd_thread_spawn(void (*entry)(void *), void *arg);
void  rd_thread_join(void *t);

/* ---- byte buffer for SSP frames (models std::vector<uint8_t>) ------------ */
typedef struct ssp_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} ssp_buf;
void ssp_buf_init(ssp_buf *b);
void ssp_buf_push(ssp_buf *b, uint8_t v);   /* push_back w/ growth */
void ssp_buf_free(ssp_buf *b);

#endif /* RIDE_COMMON_H */
