/*
 * update_common.h — shared types/interfaces for the reconstructed `update` modules
 *
 * OEM: /usr/bin/update (devices/main/update + devices/main/common). The C++ OTA
 * service is reconstructed behaviour-oriented (per-TU compilable, NOT a linkable
 * rebuild): the std::string / nlohmann::json / vm-CAN / mosquitto framework is
 * *modelled* through the interfaces below; only the VanMoof algorithms (manifest
 * parse, supplier dispatch, the page-CRC + DFU flash protocols) are translated.
 */
#ifndef UPDATE_COMMON_H
#define UPDATE_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- logger (OEM common_logf @0x16c9a0) ------------------------------- */
enum log_level { LOG_ERR = 1, LOG_WARN = 2, LOG_INFO = 3 };
void common_logf(const char *file, int line, int level, const char *fmt, ...);

/* ---- update enums (devices/main/update) ------------------------------- */
/* Per-device flash result (BackgroundUpdate maps these to display strings). */
enum update_result {
    UPD_OK          = 0,
    UPD_FAILED      = 1,
    UPD_REFUSED     = 2,
    UPD_TIMEOUT     = 3,
    UPD_SKIPPED     = 4,
};

/* A/B boot-control stage carried in the boot-control flag file. */
enum update_stage {
    STAGE_UNKNOWN = -1,
    STAGE_NONE    = 0,
    STAGE_1       = 1,
    STAGE_2       = 2,
    STAGE_ROLLBACK_3 = 3,
    STAGE_ROLLBACK_4 = 4,
};

/* ---- framework model (string / json / transport) ---------------------
 * These mirror the OEM's STL/vm plumbing at a behaviour level. The real
 * implementations live in the service runtime; reconstructed modules call them.
 */

/* tiny string helpers (model std::string operations used by the algorithms) */
bool   str_contains(const char *haystack, const char *needle);   /* std::string::find != npos */
size_t str_split(const char *s, const char *sep, char out[][64], size_t max); /* split into fields */

/* nlohmann::json modelled as an opaque handle + typed getters */
typedef struct json json_t;
bool        json_get_int(const json_t *j, const char *key, int *out);
bool        json_get_bool(const json_t *j, const char *key, bool *out);
const char *json_get_str(const json_t *j, const char *key);

/* CRC primitives used by the flash protocols (lib/common). */
uint16_t crc16_ccitt(uint16_t seed, const uint8_t *data, size_t len);
uint32_t crc32_update(uint32_t seed, const uint8_t *data, size_t len);

/* byte-serial transport (C2000 SCI/DFU): vtable-modelled putc/getc. */
typedef struct serial_transport serial_transport;
int  serial_putc(serial_transport *t, uint8_t b);
int  serial_getc(serial_transport *t, uint8_t *out, int timeout_ms); /* -1 on timeout */
void serial_open(serial_transport *t);
void serial_close(serial_transport *t);

#endif /* UPDATE_COMMON_H */
