/*
 * logging_server.c — VanMoof S5 i.MX8 `logging` service core: the log-dictionary
 * loader/parser and the compact-log expansion handler. Behaviour-oriented C
 * translation of the OEM AArch64 decompilation (program "logging", base
 * 0x100000; logging_service ctor @0x10b2a0, on-message handler @0x1127a0).
 *
 * The std::map / std::string / std::ifstream / nlohmann-json / vm transport are
 * VENDOR — modelled here (the dictionary is a growable array; JSON is emitted
 * with stdio). The VanMoof logic — the `device|file|line|LEVEL|message` config
 * format, the back-to-front ×0x1003f path hash, the (device,hash,line) key, the
 * dedup/ignored-line diagnostics, and the JSON output schema — is reproduced.
 */
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define LOG_SRC "devices/main/logging/src/logging_server.cpp"

/* ======================================================================== *
 * device-id -> name (OEM FUN_00112d00) — the shared vm role enum.
 * ======================================================================== */
const char *log_device_name(uint8_t device)
{
    switch (device) {
    case 0x00: return "CORE_SERVICES";
    case 0x08: return "HEARTBEAT";
    case 0x82: return "POWER";
    case 0x84: return "RIDE";
    case 0x87: return "LOGGING";
    case 0x88: return "UX";
    case 0x8a: return "UPDATE";
    case 0x8b: return "MQTT_OD_BRIDGE";
    case 0x8c: return "MQTT_FTP";
    case 0x8d: return "MOTOR_CONTROL";
    case 0x8f: return "IMX8_BRIDGE";
    case 0x90: return "BLE";
    case 0x91: return "MODEM";
    case 0xa1: return "MOTOR_SENSOR";
    case 0xa2: return "POWER_PEDAL";
    case 0xa3: return "POWER_CONTROL";
    case 0xa4: return "BATTERY_PRIMARY";
    case 0xa5: return "BATTERY_SECONDARY";
    case 0xa7: return "CHARGER";
    case 0xc0: return "USER_ECU";
    case 0xc1: return "ELOCK";
    case 0xc2: return "ESHIFTER";
    case 0xc3: return "REARLIGHT";
    case 0xc4: return "FRONTLIGHT";
    case 0xe0: return "PHONE";
    case 0xe2: return "BACKOFFICE";
    case 0xfb: return "PING";
    case 0xfc: return "DUMMY";
    case 0xfd: return "BATTERY_TEST";
    case 0xfe: return "RASPBERRY";
    case 0xff: return "TEST";
    default:   return "invalid enum value";
    }
}

/* ======================================================================== *
 * path normalisation + hash
 * ======================================================================== */

/* strip a leading "CMAKE_SOURCE_DIR/" (0x12fae0) or "devices/" (0x12faf8). */
const char *log_normalise_path(const char *path)
{
    static const char k_cmake[] = "CMAKE_SOURCE_DIR/";
    static const char k_devices[] = "devices/";
    if (strncmp(path, k_cmake, sizeof k_cmake - 1) == 0)
        return path + (sizeof k_cmake - 1);
    if (strncmp(path, k_devices, sizeof k_devices - 1) == 0)
        return path + (sizeof k_devices - 1);
    return path;
}

/*
 * Polynomial hash over the path, processed BACK-TO-FRONT, multiplier 0x1003f.
 * OEM: uVar28 = byte + uVar28 * 0x1003f, iterating from the last byte to the
 * first. Empty string hashes to 0.
 */
uint32_t log_path_hash(const char *path)
{
    const char *end = path + strlen(path);
    uint32_t h = 0;
    while (end != path) {
        --end;
        h = (uint32_t)(unsigned char)*end + h * 0x1003fu;
    }
    return h;
}

/* ======================================================================== *
 * the dictionary (OEM std::map<{line,hash,device}, entry>; modelled as a
 * growable array with keyed lookup + dedup).
 * ======================================================================== */
struct log_dict {
    log_entry *e;
    size_t     n;
    size_t     cap;
};

static log_entry *dict_find(log_dict *d, uint8_t device, uint32_t hash, int line)
{
    size_t i;
    for (i = 0; i < d->n; i++)
        if (d->e[i].device == device && d->e[i].file_hash == hash &&
            d->e[i].line == line)
            return &d->e[i];
    return NULL;
}

/* parse one "device|file|line|LEVEL|message" line into the dictionary. */
static void dict_parse_line(log_dict *d, char *line)
{
    char *f[5];
    int   nf = 0;
    char *p = line;

    /* split on '|' into up to 5 fields */
    f[nf++] = p;
    while (nf < 5 && (p = strchr(p, '|')) != NULL) {
        *p++ = '\0';
        f[nf++] = p;
    }
    if (nf < 5) {
        common_logf(LOG_SRC, 0x45, LOG_INF, "log config line ignored: %s", line);
        return;
    }

    {
        uint8_t  device = (uint8_t)strtol(f[0], NULL, 10);
        uint32_t hash   = log_path_hash(f[1]);     /* over the raw field-1 path */
        int      ln     = (int)strtol(f[2], NULL, 10);
        const char *file = log_normalise_path(f[1]);

        if (dict_find(d, device, hash, ln) != NULL) {
            common_logf(LOG_SRC, 0x72, LOG_INF,
                        "Duplicate log config for file %s (%d), line %d",
                        f[1], hash, ln);
            return;
        }
        if (d->n == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 256;
            d->e = (log_entry *)realloc(d->e, d->cap * sizeof *d->e);
        }
        {
            log_entry *e = &d->e[d->n++];
            e->device    = device;
            e->file_hash = hash;
            e->line      = ln;
            snprintf(e->level, sizeof e->level, "%s", f[3]);
            snprintf(e->file,  sizeof e->file,  "%s", file);
            snprintf(e->msg,   sizeof e->msg,   "%s", f[4]);
        }
    }
}

log_dict *log_dict_load(const char *config_path)
{
    log_dict *d = (log_dict *)calloc(1, sizeof *d);
    char     *text = NULL;
    char     *save, *line;

    if (!d)
        return NULL;

    if (config_path == NULL || config_path[0] == '\0') {
        /* compiled-in default (DAT_0012fe70) */
        common_logf(LOG_SRC, 0x3c, LOG_WRN, "Using the compiled-in log configuration");
        text = strdup(g_default_log_config ? g_default_log_config : "");
    } else {
        FILE *fp;
        long  n;
        common_logf(LOG_SRC, 0x38, LOG_WRN, "Reading log configuration from '%s'", config_path);
        fp = fopen(config_path, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (n > 0 && (text = (char *)malloc((size_t)n + 1)) != NULL) {
                size_t got = fread(text, 1, (size_t)n, fp);
                text[got] = '\0';
            }
            fclose(fp);
        }
    }

    /* split on '\n' and parse each line */
    for (line = text ? strtok_r(text, "\n", &save) : NULL;
         line != NULL;
         line = strtok_r(NULL, "\n", &save))
        dict_parse_line(d, line);

    free(text);
    return d;
}

const log_entry *log_dict_lookup(const log_dict *d, uint8_t device,
                                 uint32_t file_hash, int line)
{
    return dict_find((log_dict *)d, device, file_hash, line);
}

void log_dict_free(log_dict *d)
{
    if (d) {
        free(d->e);
        free(d);
    }
}

/* ======================================================================== *
 * the compact-log expansion handler (OEM @0x1127a0 -> vm callback)
 *
 * Looks up (device, file-path-hash, line); on hit, formats the message with the
 * incoming args; on miss, file/msg become "???". Emits a JSON object to the
 * output stream (std::cout): the OEM builds it with nlohmann-json — fields
 * "ts" ("%H:%M:%S"), "file_id" (the device id), "file", "line", "level", "msg".
 * Modelled here with stdio; the --filter (lower-cased device/name) gates output.
 * ======================================================================== */
void logging_on_log_message(logging_service *s, uint8_t device,
                            uint32_t file_hash, int line, const char *args_fmt)
{
    const log_entry *e = log_dict_lookup(s->dict, device, file_hash, line);
    const char *file  = e ? e->file  : "???";   /* DAT_0012fc14 "???" */
    const char *level = e ? e->level : "???";
    const char *msg   = e ? e->msg   : "???";
    char        ts[16];
    time_t      now = time(NULL);
    struct tm   tmv;

    /* --filter: only emit messages from the matching device/name (modelled). */
    if (s->has_filter) {
        char name[64];
        size_t i;
        snprintf(name, sizeof name, "%s", log_device_name(device));
        for (i = 0; name[i]; i++)
            name[i] = (char)tolower((unsigned char)name[i]);
        if (strcmp(name, s->filter) != 0)
            return;
    }

    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);   /* DAT_0012fbb0 */

    (void)args_fmt;   /* the OEM substitutes the incoming printf args into msg */

    /* emit the JSON log line to the output stream (std::cout). */
    fprintf(s->out ? (FILE *)s->out : stdout,
            "{\"ts\":\"%s\",\"file_id\":%u,\"file\":\"%s\",\"line\":%d,"
            "\"level\":\"%s\",\"msg\":\"%s\"}\n",
            ts, (unsigned)device, file, line, level, msg);
}

/* ======================================================================== *
 * service lifecycle
 * ======================================================================== */
void logging_service_init(logging_service *s, mqtt_client *mqtt, vm_ctx *can,
                          ostream *out, const char *config_path,
                          bool service_mode, const char *filter)
{
    size_t i;

    s->service_mode = service_mode;
    s->mqtt = mqtt;
    s->can  = can;
    s->out  = out;                       /* OEM: param_8 or &std::cout */

    s->has_filter = (filter != NULL && filter[0] != '\0');
    s->filter[0] = '\0';
    if (s->has_filter) {
        snprintf(s->filter, sizeof s->filter, "%s", filter);
        for (i = 0; s->filter[i]; i++)   /* lower-case (OEM tolower loop) */
            s->filter[i] = (char)tolower((unsigned char)s->filter[i]);
    }

    /* register the per-message handler on the vm transport (FUN_001127d0);
     * throws std::runtime_error("Failed on a VM call '<name>': <rc>") on error. */

    /* load + parse the dictionary (config_path, or compiled-in default). */
    s->dict = log_dict_load(config_path);
}

void logging_service_run(logging_service *s)
{
    /* the common::Service base blocks here, dispatching transport messages to
     * logging_on_log_message until shutdown. Modelled as a no-op stand-in. */
    (void)s;
}

void logging_service_deinit(logging_service *s)
{
    /* OEM 0x10b250: tear down the registered handler + free the dictionary. */
    log_dict_free(s->dict);
    s->dict = NULL;
}
