/*
 * mqtt_ftp.h — model for the VanMoof S5 i.MX8 `mqtt-ftp-service`
 * (pkg vmxs5-embedded-mqtt-ftp). Behaviour-oriented C.
 *
 * A FILE-TRANSFER-OVER-MQTT server (`MqttFtpService`): it subscribes to
 * `ftp_server/command`, parses a JSON command, reads/writes a file under a
 * mandatory destination folder in CRC32-checked chunks, and publishes JSON
 * results on `ftp_server/reply` (+ `ftp_server/file_finished`). Used by the
 * fleet for log/cert/config push-pull (rooted at /tmp by the systemd unit).
 *
 * The common::MQTTClient, common::CRC32, IClock, the CAN transport-protocol
 * multiframe assembler (lib/src/tp/tp.c), std::filesystem/fstream and
 * nlohmann-json are VENDOR — modelled as opaque externs. OEM addresses are
 * quoted in the .c. Program "mqtt-ftp-service", AArch64, image base 0x100000.
 */
#ifndef MQTT_FTP_H
#define MQTT_FTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common framework: opaque handles (vendor) ------------------------- */
typedef struct mqtt_client  mqtt_client;   /* common::IMQTTClient (mosquitto) */
typedef struct clock_iface  clock_iface;   /* common::IClock */
typedef struct json         json;          /* nlohmann::json command/reply doc */

void   common_logf(const char *file, int line, int level, const char *fmt, ...);
enum { LOG_DBG = 1, LOG_INF = 2, LOG_WRN = 3, LOG_ERR = 4 };

/* common::CRC32 (vendor) over a byte range. */
uint32_t crc32_compute(const void *data, size_t len);

/* mqtt publish/subscribe (the common::IMQTTClient vtable). */
void mqtt_subscribe(mqtt_client *c, const char *topic,
                    void (*cb)(void *ctx, const char *topic, const json *msg),
                    void *ctx);
void mqtt_publish_json(mqtt_client *c, const char *topic, const json *doc);

/* nlohmann-json accessors (modelled). */
const char *json_str (const json *d, const char *key, const char *dflt);
long        json_int (const json *d, const char *key, long dflt);
bool        json_bool(const json *d, const char *key, bool dflt);
size_t      json_bytes(const json *d, const char *key, void *out, size_t cap); /* "data" */
json       *json_new(void);
void        json_set_str (json *d, const char *key, const char *v);
void        json_set_int (json *d, const char *key, long v);
void        json_set_bool(json *d, const char *key, bool v);
void        json_free(json *d);

/* ---- topics + error tokens (DAT, verbatim) ----------------------------- */
#define FTP_TOPIC_COMMAND       "ftp_server/command"        /* sub  (0x12f028) */
#define FTP_TOPIC_REPLY         "ftp_server/reply"          /* pub  (0x12f040) */
#define FTP_TOPIC_FILE_FINISHED "ftp_server/file_finished"  /* pub  (0x12f058) */
#define FTP_ERR_FILE_ACCESS     "FTP_ERR_FILE_ACCESS"       /*      (0x12ed60) */

/* ---- the service ------------------------------------------------------- */
typedef struct mqtt_ftp_service {
    mqtt_client *mqtt;                 /* +0x08 */
    char         dest_folder[256];     /* +0x30 destination folder (mandatory) */
    clock_iface *clock;                /* +0x58 */
    uint32_t     chunk_size;           /* +0x60 default 0x200 = 512 */
    bool         verbose;
} mqtt_ftp_service;

/* MqttFtpService(IMQTTClient, std::filesystem::path dest, IClock, uint chunk).
 * Subscribes ftp_server/command; logs "Starting MQTTFTPServer". (OEM 0x10ab50) */
void mqtt_ftp_service_init(mqtt_ftp_service *s, mqtt_client *mqtt,
                           const char *dest_folder, clock_iface *clock,
                           uint32_t chunk_size);                      /* 0x10ab50 */
void mqtt_ftp_service_run(mqtt_ftp_service *s);
void mqtt_ftp_service_deinit(mqtt_ftp_service *s);                    /* 0x10a980 */

/* the ftp_server/command handler (registered at ctor, OEM cb FUN_0010a920). */
void mqtt_ftp_on_command(void *ctx, const char *topic, const json *cmd);

#endif /* MQTT_FTP_H */
