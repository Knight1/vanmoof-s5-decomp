/*
 * logging.h — model for the VanMoof S5 i.MX8 `logging` service
 * (pkg vmxs5-embedded-logging). Behaviour-oriented C.
 *
 * The on-board LOG COLLECTOR / dictionary expander. Sub-ECUs and services emit
 * COMPACT log references — (device-id, source-file-path-hash, line, args) — over
 * the vm/CAN transport (to save bandwidth on the tiny MCUs). `logging` holds a
 * dictionary (the "log config": `device|file|line|LEVEL|message` lines, loaded
 * from an optional log_config.txt or a compiled-in default), looks each ref up,
 * formats the message with its args, and emits a JSON line to stdout — which
 * systemd journald captures into /var/log on the eMMC-backed mount.
 *
 * The vm/CAN transport, common::MQTTClient, std::map/string and nlohmann-json
 * are VENDOR (modelled as opaque externs). OEM addresses are quoted in the .c.
 * Program "logging", AArch64, image base 0x100000.
 */
#ifndef LOGGING_H
#define LOGGING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- common framework: opaque handles (vendor) ------------------------- */
typedef struct vm_ctx       vm_ctx;        /* libvm SocketCAN/CANopen ctx */
typedef struct mqtt_client  mqtt_client;   /* common::MQTTClient (mosquitto) */
typedef struct ostream      ostream;       /* std::ostream (default: std::cout) */

/* platform log (common::logf): file, line, level{1=DBG 2=INF 3=WRN 4=ERR}. */
void common_logf(const char *file, int line, int level, const char *fmt, ...);
enum log_level { LOG_DBG = 1, LOG_INF = 2, LOG_WRN = 3, LOG_ERR = 4 };

/* ---- device-id -> name (FUN_00112d00; the shared vm role enum) ---------- *
 * 0=CORE_SERVICES, 8=HEARTBEAT, 0x82=POWER, 0x84=RIDE, 0x87=LOGGING,
 * 0x88=UX, 0x8a=UPDATE, 0x8d=MOTOR_CONTROL, 0x90=BLE, 0x91=MODEM, 0xa4=BATTERY_PRIMARY,
 * 0xa7=CHARGER, 0xc1=ELOCK, … 0xfb=PING, 0xff=TEST; else "invalid enum value". */
const char *log_device_name(uint8_t device);                         /* 0x112d00 */

/* ---- log dictionary ---------------------------------------------------- *
 * One parsed config entry, keyed by (device, file-path hash, line).        */
typedef struct log_entry {
    uint8_t  device;        /* field 0 (strtol base 10) — the source ECU/role */
    uint32_t file_hash;     /* polynomial hash of the normalised file path    */
    int      line;          /* field 2 (strtol base 10)                       */
    char     level[8];      /* field 3: "DBG"/"INF"/"WRN"/"ERR"               */
    char     file[128];     /* field 1, normalised                            */
    char     msg[192];      /* field 4: the message format (printf-style)     */
} log_entry;

typedef struct log_dict log_dict;          /* std::map<key, log_entry> (modelled) */

/* the compiled-in default config text (DAT_0012fe70) — a big newline-joined
 * "device|file|line|LEVEL|message" blob; modelled as an opaque extern. */
extern const char *g_default_log_config;                             /* 0x12fe70 */

/* normalise a source path: strip a leading "CMAKE_SOURCE_DIR/" or "devices/". */
const char *log_normalise_path(const char *path);
/* polynomial hash over the (normalised) path, processed back-to-front, ×0x1003f. */
uint32_t    log_path_hash(const char *path);

/* load + parse the dictionary (from `config_path`, or the compiled-in default
 * when config_path is NULL/empty). Reproduces the '\n'/'|' split, the dedup
 * warning, and the "log config line ignored" path. */
log_dict   *log_dict_load(const char *config_path);
void        log_dict_free(log_dict *d);
/* look up an incoming reference; returns NULL on miss (-> "???"). */
const log_entry *log_dict_lookup(const log_dict *d, uint8_t device,
                                 uint32_t file_hash, int line);

/* ---- the logging service ----------------------------------------------- */
typedef struct logging_service {
    bool         service_mode;   /* -s/--service                     */
    char         filter[64];     /* -f/--filter <address|name>, lower-cased; "" = off */
    bool         has_filter;
    vm_ctx      *can;            /* transport the compact logs arrive on */
    mqtt_client *mqtt;
    ostream     *out;           /* std::cout, or an injected stream    */
    log_dict    *dict;          /* the loaded dictionary               */
} logging_service;

void logging_service_init(logging_service *s, mqtt_client *mqtt, vm_ctx *can,
                          ostream *out, const char *config_path,
                          bool service_mode, const char *filter);     /* 0x10b2a0 */
void logging_service_run(logging_service *s);                         /* (base run) */
void logging_service_deinit(logging_service *s);                      /* 0x10b250 */

/* the per-message handler (registered on the transport @0x1127a0): expand a
 * compact reference and emit the JSON line. */
void logging_on_log_message(logging_service *s, uint8_t device,
                            uint32_t file_hash, int line, const char *args_fmt);

#endif /* LOGGING_H */
