/*
 * store.c — external-storage page-cache.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   store_flush @ 0x00008876  (write-back the cached page, advance/invalidate)
 *   store_load  @ 0x000088d2  (flush, then prefetch a page range into the cache)
 *
 * Storage is addressed in 0x200-byte pages; page index N lives at byte address
 * (N + 0xe0) * 0x200, valid indices 0..0xd9. The page buffer is programmed back
 * with bus_page_program and pages are fetched with bus_transfer_token.
 */

#include <stdint.h>

#include "store.h"
#include "bus.h"        /* bus_page_program, bus_transfer_token */
#include "util.h"       /* vmem_set */

#define STORE_PAGE_SIZE     0x200u
#define STORE_PAGE_BASE     0xe0u      /* page N -> byte (N + 0xe0) * 0x200 */
#define STORE_PAGE_LAST     0xd9u      /* highest valid page index         */
#define STORE_PAGE_INVALID  0xffffu

/*
 * The storage cache object. Only the OEM-touched fields are modeled; the
 * leading region is opaque padding so the byte offsets match.
 */
typedef struct store {
    uint8_t  _pad00[0x3c];  /* +0x00..+0x3b                                 */
    uint8_t  enable;        /* +0x3c nonzero when the cache is usable       */
    uint8_t  _pad3d[3];     /* +0x3d..+0x3f                                 */
    uint8_t *buf;           /* +0x40 0x200-byte page buffer                 */
    uint16_t page;          /* +0x44 cached page index (0xffff = invalid)   */
    uint16_t count;         /* +0x46 cached / remaining page count          */
} store_t;

/* The storage controller; only +0x06/+0x08/+0x10/+0x15/+0x18 are touched. */
typedef struct store_ctrl {
    uint8_t  _pad00[6];     /* +0x00..+0x05                                 */
    uint16_t cur_page;      /* +0x06 last requested start page              */
    uint16_t _w08;          /* +0x08 cleared on load                        */
    uint8_t  _pad0a[6];     /* +0x0a..+0x0f                                 */
    uint32_t _w10;          /* +0x10 cleared on load                        */
    uint8_t  _b14;          /* +0x14                                        */
    uint8_t  _b15;          /* +0x15 cleared on load                        */
    uint8_t  _pad16[2];     /* +0x16..+0x17                                 */
    store_t *store;         /* +0x18 the storage object                     */
} store_ctrl_t;

/*
 * store_flush — write the cached page back and advance the window. // 0x00008876
 */
int store_flush(struct store *st)
{
    int      failed;
    uint16_t count;

    if (st->page == STORE_PAGE_INVALID) {       /* ldrh +0x44 ; cmp 0xffff */
        return 0;                               /* nothing cached */
    }

    failed = (bus_page_program(st,
                  (uint32_t)(st->page + STORE_PAGE_BASE) * STORE_PAGE_SIZE,
                  st->buf) != 0);               /* (page+0xe0)*0x200, buf */
    count = st->count;                          /* ldrh +0x46 */

    if (count <= 1) {                           /* cmp #1 ; bls */
        /* OEM stores 0xffff as one 32-bit word at +0x44 -> page=0xffff, count=0 */
        st->page  = STORE_PAGE_INVALID;
        st->count = 0;
    } else {
        uint16_t next = (uint16_t)(st->page + 1);
        st->page  = STORE_PAGE_INVALID;         /* str.w 0xffff (clears both) ... */
        st->count = 0;
        vmem_set(st->buf, 0xff, STORE_PAGE_SIZE);
        st->page  = next;                       /* ... then advance page */
        st->count = (uint16_t)(count - 1);      /* and decrement count   */
    }
    return failed;
}

/*
 * store_load — flush, then load `range` pages into the cache window. // 0x000088d2
 */
int store_load(struct store_ctrl *ctrl, uint32_t arg2,
               const store_range_t *range, uint32_t arg4)
{
    store_t *st = ctrl->store;                  /* ldr +0x18 */
    uint16_t start;
    uint16_t count;

    (void)arg2;     /* OEM never reads r1 */
    (void)arg4;     /* OEM never reads r3 */

    ctrl->_w10 = 0;                             /* str +0x10  (before the store check) */
    ctrl->_b15 = 0;                             /* strb +0x15 */

    if (st == (store_t *)0 || st->enable == 0) {
        return -1;
    }

    start = range->start;                       /* ldrh [range] */
    if (start > STORE_PAGE_LAST) {              /* uxth ; cmp 0xd9 ; bhi */
        return -1;
    }
    count = range->count;                       /* ldrh [range+2] */

    if (store_flush(st) != 0) {
        return -1;
    }

    if (count != 0) {                           /* cbz r7 */
        uint32_t byte_addr;
        uint16_t i;

        if ((uint32_t)start + count > STORE_PAGE_LAST) {  /* count+start > 0xd9 */
            count = (uint16_t)(0xdau - start);            /* clamp to end at 0xd9 */
        }
        byte_addr = (uint32_t)(start + STORE_PAGE_BASE) * STORE_PAGE_SIZE;
        i = 0;
        do {
            if (bus_transfer_token(st, byte_addr) != 0) {
                return -1;
            }
            i++;
            byte_addr += STORE_PAGE_SIZE;
        } while (count > i);                     /* cmp r7,r3 ; bhi */

        /* invalidate, clear the buffer, then latch the new [start, count) window */
        st->page  = STORE_PAGE_INVALID;          /* str 0xffff (clears both) */
        st->count = 0;
        vmem_set(st->buf, 0xff, STORE_PAGE_SIZE);
        st->page  = start;                       /* strh +0x44 */
        st->count = count;                       /* strh +0x46 */
    }

    ctrl->cur_page = start;                      /* strh +0x06 */
    ctrl->_w08 = 0;                              /* strh +0x08 */
    return 0;
}
