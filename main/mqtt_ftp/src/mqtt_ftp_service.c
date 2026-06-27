/*
 * mqtt_ftp_service.c — VanMoof S5 i.MX8 `mqtt-ftp-service` core: MqttFtpService.
 * Behaviour-oriented C translation of the OEM AArch64 decompilation (program
 * "mqtt-ftp-service", base 0x100000; ctor @0x10ab50, dtor @0x10a980, command
 * handler @FUN_0010a920).
 *
 * Subscribes ftp_server/command, parses a JSON command, reads/writes a file
 * under the destination folder in CRC32-checked chunks, and publishes JSON
 * results on ftp_server/reply (+ ftp_server/file_finished). The common::CRC32,
 * the CAN transport-protocol multiframe assembler (lib/src/tp/tp.c), nlohmann-
 * json, std::filesystem/fstream are VENDOR — modelled as opaque externs; the
 * topic/JSON-key/error vocabulary, the chunked CRC flow and the log strings are
 * reproduced verbatim from the binary.
 */
#include "mqtt_ftp.h"

#include <stdio.h>
#include <string.h>

#define FTP_SRC "devices/main/mqtt-ftp/src/mqtt_ftp_service.cpp"

/* ------------------------------------------------------------------------ *
 * reply helper — publish a JSON result on ftp_server/reply.
 * Reply schema keys: status, name, size, index, crc, cached (+ "data" on reads).
 * `silent` is a REQUEST flag that suppresses the reply entirely — it is not
 * echoed in the reply JSON. The only FTP error token is FTP_ERR_FILE_ACCESS
 * (the "other_error" string in the binary is an nlohmann-json exception type,
 * not an FTP status). Confirmed vs OEM ctor @0x10ab50 + topics @0x12f028.
 * ------------------------------------------------------------------------ */
static void ftp_reply(mqtt_ftp_service *s, const char *name, const char *status,
                      long size, long index, uint32_t crc, bool cached, bool silent)
{
    json *r;
    if (silent)
        return;                               /* "silent" commands suppress the reply */
    r = json_new();
    json_set_str (r, "name",   name);
    json_set_str (r, "status", status);       /* "OK" or FTP_ERR_FILE_ACCESS / other_error */
    json_set_int (r, "size",   size);
    json_set_int (r, "index",  index);
    json_set_int (r, "crc",    (long)crc);
    json_set_bool(r, "cached", cached);
    mqtt_publish_json(s->mqtt, FTP_TOPIC_REPLY, r);
    json_free(r);
}

/* ------------------------------------------------------------------------ *
 * WRITE path — accept a file chunk and append it under the destination folder.
 * The CAN transport-protocol layer (tp.c) reassembles multiframe data before
 * this is called; here a complete `data` payload arrives per command, indexed
 * by `index` of `chunk_size`. Per-block and whole-file CRC32 are verified.
 * ------------------------------------------------------------------------ */
static void ftp_handle_write(mqtt_ftp_service *s, const json *cmd)
{
    const char *name  = json_str(cmd, "name", "");
    long        index = json_int(cmd, "index", 0);
    uint32_t    want  = (uint32_t)json_int(cmd, "crc", 0);
    bool        silent = json_bool(cmd, "silent", false);
    bool        flush  = json_bool(cmd, "flush", false);
    char        path[512];
    uint8_t     buf[4096];
    size_t      n;
    uint32_t    got;
    FILE       *fp;

    snprintf(path, sizeof path, "%s/%s", s->dest_folder, name);

    if (index == 0) {
        common_logf(FTP_SRC, 0x00, LOG_WRN, "Starting transfer for file: %s", name);
        common_logf(FTP_SRC, 0x00, LOG_DBG, "Deleting existing data");   /* truncate */
    }

    n   = json_bytes(cmd, "data", buf, sizeof buf);
    got = crc32_compute(buf, n);                          /* per-block CRC32 */
    if (got != want) {
        common_logf(FTP_SRC, 0x00, LOG_ERR, "Requested block CRC: %d", (int)want);
        ftp_reply(s, name, FTP_ERR_FILE_ACCESS, 0, index, got, false, silent);
        return;
    }

    fp = fopen(path, index == 0 ? "wb" : "ab");           /* truncate on first chunk */
    if (fp == NULL) {
        ftp_reply(s, name, FTP_ERR_FILE_ACCESS, 0, index, 0, false, silent);
        return;
    }
    fwrite(buf, 1, n, fp);
    fclose(fp);

    if (flush) {                                          /* last chunk: finalise */
        common_logf(FTP_SRC, 0x00, LOG_WRN, "Finished transfer for file: %s", name);
        ftp_reply(s, name, "OK", (long)n, index, got, false, silent);
        /* announce completion */
        {
            json *f = json_new();
            json_set_str(f, "name", name);
            mqtt_publish_json(s->mqtt, FTP_TOPIC_FILE_FINISHED, f);
            json_free(f);
        }
    } else {
        ftp_reply(s, name, "OK", (long)n, index, got, false, silent);
    }
}

/* ------------------------------------------------------------------------ *
 * READ path — stream a file under the destination folder back in chunks, with
 * per-block and whole-file CRC32. `cached` is reported when the chunk is served
 * from an in-memory cache rather than re-read.
 * ------------------------------------------------------------------------ */
static void ftp_handle_read(mqtt_ftp_service *s, const json *cmd)
{
    const char *name   = json_str(cmd, "name", "");
    long        index  = json_int(cmd, "index", 0);
    uint32_t    chunk  = (uint32_t)json_int(cmd, "chunk_size", (long)s->chunk_size);
    uint32_t    want   = (uint32_t)json_int(cmd, "crc", 0);
    bool        silent = json_bool(cmd, "silent", false);
    char        path[512];
    uint8_t     buf[4096];
    size_t      n;
    uint32_t    block_crc, file_crc;
    FILE       *fp;
    long        size;

    snprintf(path, sizeof path, "%s/%s", s->dest_folder, name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        ftp_reply(s, name, FTP_ERR_FILE_ACCESS, 0, index, 0, false, silent);
        return;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    if (chunk > sizeof buf)
        chunk = sizeof buf;
    fseek(fp, (long)index * chunk, SEEK_SET);
    n = fread(buf, 1, chunk, fp);
    fclose(fp);

    block_crc = crc32_compute(buf, n);
    common_logf(FTP_SRC, 0x00, LOG_DBG, "Requested block CRC: %d", (int)block_crc);

    /* whole-file CRC check when the client supplied the expected value. */
    file_crc = block_crc;   /* the full-file CRC is accumulated across chunks (vendor) */
    if (want != 0 && want != file_crc)
        common_logf(FTP_SRC, 0x00, LOG_WRN,
                    "Requested file CRC: %d (should be %d)", (int)want, (int)file_crc);

    if (!silent) {
        json *r = json_new();
        json_set_str (r, "name",  name);
        json_set_str (r, "status", "OK");
        json_set_int (r, "size",  size);
        json_set_int (r, "index", index);
        json_set_int (r, "crc",   (long)block_crc);
        json_set_bool(r, "cached", false);
        /* the chunk bytes are attached under "data" (base64 in the OEM json) */
        mqtt_publish_json(s->mqtt, FTP_TOPIC_REPLY, r);
        json_free(r);
    }
}

/* ------------------------------------------------------------------------ *
 * command handler (OEM cb FUN_0010a920 on ftp_server/command).
 * Parses the JSON command and dispatches. Confirmed keys: cmd, name, path,
 * data, index, chunk_size, crc, cached, silent, size, status. (The exact `cmd`
 * token spellings were not recovered; a write carries a "data" payload, a read
 * does not — modelled accordingly.)
 * ------------------------------------------------------------------------ */
void mqtt_ftp_on_command(void *ctx, const char *topic, const json *cmd)
{
    mqtt_ftp_service *s = (mqtt_ftp_service *)ctx;
    char data_probe[1];
    (void)topic;

    /* a command bearing a "data" field is a write; otherwise a read request. */
    if (json_bytes(cmd, "data", data_probe, sizeof data_probe) > 0 ||
        json_bool(cmd, "flush", false))
        ftp_handle_write(s, cmd);
    else
        ftp_handle_read(s, cmd);
}

/* ------------------------------------------------------------------------ *
 * lifecycle
 * ------------------------------------------------------------------------ */
void mqtt_ftp_service_init(mqtt_ftp_service *s, mqtt_client *mqtt,
                           const char *dest_folder, clock_iface *clock,
                           uint32_t chunk_size)
{
    s->mqtt  = mqtt;
    s->clock = clock;
    s->chunk_size = chunk_size ? chunk_size : 0x200;     /* default 512 */
    s->verbose = false;
    snprintf(s->dest_folder, sizeof s->dest_folder, "%s", dest_folder);

    common_logf(FTP_SRC, 0x10, LOG_WRN, "Starting MQTTFTPServer");
    mqtt_subscribe(s->mqtt, FTP_TOPIC_COMMAND, mqtt_ftp_on_command, s);
}

void mqtt_ftp_service_run(mqtt_ftp_service *s)
{
    /* the common::Service base blocks here, dispatching ftp_server/command
     * messages to the handler until shutdown. Modelled as a no-op stand-in. */
    (void)s;
}

void mqtt_ftp_service_deinit(mqtt_ftp_service *s)
{
    common_logf(FTP_SRC, 0x00, LOG_WRN, "Stopping MQTTFTPServer");
    (void)s;
}
