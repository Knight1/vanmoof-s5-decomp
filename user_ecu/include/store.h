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

#endif /* USER_ECU_STORE_H */
