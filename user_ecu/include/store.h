#ifndef USER_ECU_STORE_H
#define USER_ECU_STORE_H

#include <stdint.h>

/*
 * store.h — external-storage page-cache.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * A single-window page cache over an external storage device addressed in
 * 0x200-byte pages. Page index N maps to byte address (N + 0xe0) * 0x200; valid
 * page indices are 0..0xd9. The cache object holds a 0x200-byte buffer plus the
 * cached page index (0xffff = invalid) and a count; writes are flushed back with
 * bus_page_program and reads/prefetches issued with bus_transfer_token (both in
 * bus.c). The struct layouts are partial (only the OEM-touched fields are
 * modeled), so they are kept opaque here.
 */

struct store;        /* the storage cache object (buffer + cached page/count) */
struct store_ctrl;   /* a storage controller; holds a `store` at +0x18         */

/* A {start page, page count} request passed to store_load. */
typedef struct store_range {
    uint16_t start;   /* first page index (0..0xd9) */
    uint16_t count;   /* number of pages            */
} store_range_t;

/*
 * store_flush — write the currently-cached page back to storage and advance.
 * If no page is cached (index 0xffff) it is a no-op. Otherwise the page buffer
 * is programmed with bus_page_program; then, if the remaining count is <= 1 the
 * cache is invalidated, else the buffer is cleared, the page index advances and
 * the count decrements. Returns non-zero if the program failed. // 0x00008876
 */
int store_flush(struct store *st);

/*
 * store_load — flush any pending page, then load the `range` of pages into the
 * cache window. The start page must be <= 0xd9; the count is clamped so the
 * window ends at page 0xd9. Each page is fetched with bus_transfer_token; on
 * success the cache latches [start, start+count). `arg2`/`arg4` mirror the OEM's
 * unused 2nd/4th register arguments. Returns 0 on success, -1 on a disabled/NULL
 * store, an out-of-range start, or a flush/fetch failure. // 0x000088d2
 */
int store_load(struct store_ctrl *ctrl, uint32_t arg2,
               const store_range_t *range, uint32_t arg4);

/*
 * store_descriptor_read — read the 8-byte storage descriptor at byte 0x37400.
 * Fetches it via the token path; if the descriptor page is not yet provisioned
 * it falls back to a raw page read (zeroing `out` when nothing is stored, else
 * extracting the 8 bytes). On a 0 return `out` holds the 8-byte descriptor.
 * Returns 0 on success, -2 on a NULL session, -1 on an extract/log failure, or
 * the underlying read status otherwise. // 0x00006708
 */
int store_descriptor_read(void *sess, void *out);

/*
 * event_report — post a `word_count`-word event/error record (ctx + 16-bit code
 * + payload words) to the device manager's FreeRTOS event queue, when one is
 * registered. Variadic: each payload word is a uint32_t. // 0x00003eac
 */
void event_report(uint32_t ctx, uint16_t code, int word_count, ...);

/*
 * fota_image_verify — stream `length` bytes of the staged image (from byte
 * `offset` within the data region) through the hardware checksum engine and
 * publish the accumulated result to *out_crc. Returns 0 on success, 1 on a
 * store/flush/read failure, 2 on bad arguments (NULL out_crc or a span past the
 * 0x1b400-byte region). // 0x00002acc
 */
int fota_image_verify(struct store *st, uint32_t offset, uint32_t length,
                      uint32_t *out_crc);

/*
 * store_descriptor_write — write the 8-byte `descriptor` to its flash page.
 * Returns 0 on success, -1 on a prep/program/commit failure, -2 on a NULL
 * session. // 0x00006794
 */
int store_descriptor_write(void *sess, const void *descriptor);

/*
 * log_append_event — append an 8-byte event record (tagged with `flag`) to the
 * log store via store_descriptor_write, when a store handle is registered.
 * // 0x0000681c
 */
void log_append_event(uint8_t flag);

#endif /* USER_ECU_STORE_H */
