/*
 * store.c — external-storage page-cache.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   store_flush             @ 0x00008876  (write-back the cached page, advance)
 *   store_load              @ 0x000088d2  (flush, then prefetch a page range)
 *   record_table_store      @ 0x000040d4  (store a 42-byte record into an indexed SRAM table)
 *   event_report            @ 0x00003eac  (post an event/error record to the queue)
 *   flash_page_write        @ 0x0000442c  (program a page + read-back verify)
 *   flash_page_commit       @ 0x0000668c  (stage scratch + program descriptor)
 *   fota_image_verify       @ 0x00002acc  (checksum the staged image region)
 *   store_descriptor_read   @ 0x00006708  (read the 8-byte descriptor @ 0x37400)
 *   store_descriptor_write  @ 0x00006794  (write the 8-byte descriptor)
 *   log_append_event        @ 0x0000681c  (append an event record via the writer)
 *   xfer_state_log_notify   @ 0x00001884  (transfer-complete: state toggle + log + wake)
 *   xfer_waiter_notify      @ 0x00001914  (reset waiter queues flagged in a record byte)
 *   xfer_waiter_post_frame  @ 0x0000193c  (assemble a 7-byte frame, post to both waiters)
 *   timer_remaining_ticks   @ 0x000019f8  (ticks until a software timer expires)
 *
 * Storage is addressed in 0x200-byte pages; page index N lives at byte address
 * (N + 0xe0) * 0x200, valid indices 0..0xd9. The page buffer is programmed back
 * with bus_page_program and pages are fetched with bus_transfer_token. The
 * 8-byte storage descriptor sits at the fixed byte address 0x37400 (just past
 * the 0x1c000..0x37400 data region).
 */

#include <stdarg.h>
#include <stdint.h>

#include "store.h"
#include "bus.h"        /* bus_page_program/read/transfer_token, g_bus_mgr */
#include "util.h"       /* vmem_set, vmem_copy */
#include "hal.h"        /* checksum_feed */
#include "pcc.h"        /* port_clock_wait */

/* External-flash FOTA staging geometry (byte addresses; 0x200-byte pages). */
#define STORE_DESCRIPTOR_ADDR   0x37400u    /* 8-byte image descriptor page    */
#define FLASH_SCRATCH_ADDR      0x37a00u    /* scratch/commit page (next page) */
#define FLASH_DATA_BASE         0x1c000u    /* image data region base byte     */
#define FLASH_DATA_SIZE         0x1b400u    /* image data region size (111 KB) */
#define FLASH_EVENT_CTX         0xe6384427u /* subsystem id stamped in records  */

/* HW checksum/CRC engine (DAT_00002b90): +0 control, +4 seed, +8 result/data. */
#define CSUM_BASE               0x40095000u

/*
 * Indexed record table (SRAM) written by record_table_store: 0x1f5 (501) slots
 * of 0x2a bytes each, addressed slot N -> base + N * 0x2a. An out-of-range index
 * posts an event under this module's own subsystem ctx (distinct from the flash
 * FLASH_EVENT_CTX above). // pool @ 0x00004104 / 0x00004108
 */
#define REC_TABLE_BASE          0x201e1eb4u  /* DAT @0x4108: slot-0 byte address */
#define REC_TABLE_STRIDE        0x2au        /* 42-byte record / slot           */
#define REC_TABLE_SLOTS         0x1f5u       /* valid index range 0..0x1f4      */
#define REC_TABLE_EVENT_CTX     0x207a5327u  /* DAT @0x4104: subsystem id       */
#define REC_TABLE_EVENT_OOR     0x114u       /* event code on index out-of-range */

/*
 * FreeRTOS xStreamBufferSend (stream_buffer.c, vendor, deferred) — event_report
 * posts records to the device manager's event message buffer through it. The
 * decompiler renders it 5-arg; kept ABI-compatible. // 0x0000926c
 */
extern int xStreamBufferSend(void *buffer, uint32_t pos, uint32_t ticks,
                             const void *item, uint32_t len);

/*
 * Read-back verify op for flash_page_write, variant A: a fixed off-image
 * function pointer (DAT_000044b8 = 0x1300427c). Variant B uses the driver
 * vtable's page_verify slot (bus.h).
 */
extern int bus_verify_op(void *sess, uint32_t addr, uint32_t len, void *buf,
                         int *out_b, int *out_a);

/* Event-record source + store handle for log_append_event (runtime RAM). */
typedef struct log_event_src {
    uint32_t word0;     /* +0x00 -> record[0..3] */
    uint16_t half4;     /* +0x04 -> record[4..5] */
} log_event_src_t;
extern void *g_log_store_handle;                 /* *0x2000069c (DAT_0000684c) */
extern volatile log_event_src_t g_log_event_src; /* 0x0001b31f (DAT_00006850) */

/*
 * Transfer-completion waiter objects + the connection/state flag used by
 * xfer_state_log_notify (runtime SRAM; the literals are object base addresses,
 * not pointer cells). Each waiter object carries a presence byte at +0x00, a
 * 16-bit field at +0x01, and a queue/sem handle at +0x0c.
 */
#define g_xfer_waiter_a    ((volatile uint8_t *)0x200006b4u)  /* DAT_00001908 */
#define g_xfer_waiter_b    ((volatile uint8_t *)0x20000970u)  /* DAT_0000190c */
#define g_xfer_state_flag  (*(volatile uint8_t *)0x200070e9u) /* DAT_00001910 */

/*
 * xfer_waiter_reset — reset a waiter object's embedded queue and release its
 * blocked task. Recomputes the embedded queue's (waiter+0xc) write/read cursors,
 * clears the message count, sets the lock sentinels and unblocks a waiter, then
 * signals the semaphore at waiter+8. The queue-reset core is FreeRTOS
 * `xQueueGenericReset`-shaped but the wrapper (queue@+0xc, sem@+8) is
 * VanMoof-vs-vendor ambiguous — left extern, classification pending. // 0x000087fa
 */
extern void xfer_waiter_reset(void *waiter);

/*
 * rtos_sem_give — FreeRTOS queue/semaphore send (vendor, deferred). Distinct from
 * the event-buffer send xStreamBufferSend above; used to hand a completed record
 * to a blocked task. // 0x00006ec0
 */
extern int rtos_sem_give(void *q, const void *item, int pos);

/*
 * FreeRTOS critical-section / configASSERT-trap primitives (vendor, deferred) —
 * used by xfer_waiter_post_frame and timer_remaining_ticks. // 0x6454/0x6470/0x950a
 */
extern void vPortEnterCritical(void);
extern void vPortExitCritical(void);
extern void vPortRaiseBASEPRI(void);

/* Separate OEM functions defined below; forward-declared (kept un-inlined). */
void event_report(uint32_t ctx, uint16_t code, int word_count, ...);
int  record_table_store(uint32_t arg0, uint32_t arg1, const void *record,
                        uint32_t arg3);
int  flash_page_write(void *sess, uint32_t addr, void *buf);
int  flash_page_commit(void *sess, void *src);
int  store_descriptor_write(void *sess, const void *descriptor);

#define STORE_PAGE_SIZE     0x200u
#define STORE_PAGE_BASE     0xe0u      /* page N -> byte (N + 0xe0) * 0x200 */
#define STORE_PAGE_LAST     0xd9u      /* highest valid page index         */
#define STORE_PAGE_INVALID  0xffffu

/*
 * FOTA image-region byte geometry (sibling of fota_image_verify). The image
 * footer/trailer occupies the last data page (index 0xd9); its byte address is
 * (0xd9 + 0xe0) * 0x200 = 0x37200, and the writable image region spans
 * 0..0x1b3fc bytes. // pool @ 0x00002ac0
 */
#define FOTA_IMAGE_SIZE     0x1b3fcu   /* DAT @0x2ac0: writable span / bound  */
#define FOTA_FOOTER_PAGEIDX 0x100d9u   /* DAT @0x2ac4: count<<16 | page(0xd9) */
#define FOTA_FOOTER_ADDR    0x37200u   /* DAT @0x2ac8: byte addr of page 0xd9 */

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

/*
 * event_report — post an event/error record to the manager's queue. // 0x00003eac
 *
 * If the device-manager handle slot (*0x2000171c) is populated, build a 30-byte
 * record { ctx@+0, code@+4, payload[word_count]@+6 } and post it via the
 * FreeRTOS stream/message buffer at (manager+0x590) using xStreamBufferSend (with
 * the fixed position constant 0x4801). Variadic by `word_count` 32-bit words
 * (the OEM caps it at 6 by the buffer size; no bounds check — verbatim).
 */
void event_report(uint32_t ctx, uint16_t code, int word_count, ...)
{
    void *handle = *(void *volatile *)0x2000171cu;   /* DAT_00003f0c -> *(slot) */
    if (handle == (void *)0) {                        /* gate */
        return;
    }

    {
        void    *mgr   = *(void **)handle;            /* the device manager  */
        void    *queue = *(void **)((uint8_t *)mgr + 0x590); /* event queue   */
        uint8_t  record[0x1e];
        uint32_t *payload = (uint32_t *)(record + 6);
        va_list  ap;
        int      i;

        vmem_set(record, 0, 0x1e);
        va_start(ap, word_count);
        for (i = 0; i != word_count; i++) {           /* copy payload words */
            payload[i] = va_arg(ap, uint32_t);
        }
        va_end(ap);

        *(uint32_t *)(record + 0) = ctx;              /* str  ctx  @+0 */
        *(uint16_t *)(record + 4) = code;             /* strh code @+4 */

        xStreamBufferSend(queue, 0x4801u, 0, record, 0x1e); /* stream-buffer send */
    }
}

/*
 * record_table_store — store a 42-byte record into the indexed SRAM table. // 0x000040d4
 *
 * The record's first 16-bit field is the slot index; the 42 bytes that follow
 * (record + 2) are copied verbatim to REC_TABLE_BASE + index * 0x2a. Valid
 * indices are 0..0x1f4 (< REC_TABLE_SLOTS); an out-of-range index posts
 * event REC_TABLE_EVENT_OOR under this module's ctx and returns -1. Returns 0 on
 * a successful store. The leading r0/r1/r3 register arguments are part of the OEM
 * dispatch ABI but unread by the body. // push {r3,lr}
 */
int record_table_store(uint32_t arg0, uint32_t arg1, const void *record,
                       uint32_t arg3)
{
    const uint16_t *rec = (const uint16_t *)record;
    uint32_t        index = *rec;                /* ldrh [r2,#0] */

    (void)arg0;
    (void)arg1;
    (void)arg3;

    if (index < REC_TABLE_SLOTS) {               /* cmp #0x1f4 ; bls */
        vmem_copy((void *)(REC_TABLE_BASE + index * REC_TABLE_STRIDE),
                  rec + 1,                        /* record + 2 bytes */
                  REC_TABLE_STRIDE);              /* 0x984c */
        return 0;
    }

    event_report(REC_TABLE_EVENT_CTX, REC_TABLE_EVENT_OOR, 0);  /* 0x3eac */
    return -1;
}

/*
 * flash_page_write — program a 0x200-byte page at `addr` and read-back verify. // 0x0000442c
 *
 * 1) bus_transfer_token(sess, addr) — arm/select the page (fail -> code 0x31).
 * 2) bus_page_program(sess, addr, buf) — program it (fail -> code 0x38).
 * 3) read-back verify: variant A via the fixed op bus_verify_op, variant B via
 *    the driver vtable page_verify slot (+0x14); both get (sess, addr, 0x200,
 *    buf, &out_b, &out_a) with the two out-slots zeroed and unused (fail -> 0x42).
 * On any failure event_report(FLASH_EVENT_CTX, code, 2, addr, rc) and return -1.
 */
int flash_page_write(void *sess, uint32_t addr, void *buf)
{
    int      rc;
    uint16_t code;
    int      out_a = 0;       /* sp+0x10 verify out-param (unused) */
    int      out_b = 0;       /* sp+0x14 verify out-param (unused) */

    rc = bus_transfer_token(sess, addr);          /* 0x664c */
    if (rc != 0) { code = 0x31; goto fail; }

    rc = bus_page_program(sess, addr, buf);       /* 0x6610 */
    if (rc != 0) { code = 0x38; goto fail; }

    /* OEM order: stacked args are &out_b (sp+0x14) then &out_a (sp+0x10). */
    if (bus_variant_b() == 0) {                    /* 0x288c */
        rc = bus_verify_op(sess, addr, 0x200, buf, &out_b, &out_a);          /* DAT_000044b8 */
    } else {
        rc = g_bus_mgr->driver->page_verify(sess, addr, 0x200, buf, &out_b, &out_a); /* vtable +0x14 */
    }
    if (rc == 0) {
        return 0;
    }
    code = 0x42;

fail:
    event_report(FLASH_EVENT_CTX, code, 2, addr, (uint32_t)rc);   /* 0x3eac */
    return -1;
}

/*
 * flash_page_commit — stage the scratch page, then program the descriptor. // 0x0000668c
 *
 * Stages/erases the scratch page (driver vtable page_load @ +0x10; 0 => fail,
 * return -3), reads it back into `src` (bus_page_read; fail -> event 0x65, -1),
 * programs the descriptor page at 0x37400 from `src` (flash_page_write; fail
 * -> -1), then tokens the scratch page (fail -> event 0x73, return 0 regardless).
 */
int flash_page_commit(void *sess, void *src)
{
    int rc;

    if (g_bus_mgr->driver->page_load(sess, FLASH_SCRATCH_ADDR, 0x200) == 0) {
        return -3;                                /* mvn r4,#2 */
    }

    rc = bus_page_read(sess, FLASH_SCRATCH_ADDR, src, 0x200);    /* 0x29b4 */
    if (rc != 0) {
        event_report(FLASH_EVENT_CTX, 0x65, 2, 0, (uint32_t)rc);
        return -1;
    }

    rc = flash_page_write(sess, STORE_DESCRIPTOR_ADDR, src);     /* 0x442c @ 0x37400 */
    if (rc != 0) {
        return -1;                                /* r4 forced to 0xffffffff */
    }

    rc = bus_transfer_token(sess, FLASH_SCRATCH_ADDR);          /* 0x664c */
    if (rc != 0) {
        event_report(FLASH_EVENT_CTX, 0x73, 2, 0, (uint32_t)rc);
    }
    return 0;                                     /* token-error tail returns r4 (==0) */
}

/*
 * fota_image_write — write image bytes into the staged region. // 0x000029f8
 *
 * The write counterpart of fota_image_verify (sibling pool @0x2ac0). `off` is a
 * byte offset within the writable image span (0..0x1b3fc); `src`/`len` are the
 * source bytes. Requires the store enabled (+0x3c). Two paths:
 *
 *   A) Footer prime + write (page index invalid AND off == 0x1b3fc): clear the
 *      cached count, fill the page buffer with 0xff, latch the footer page index
 *      (0x100d9 -> page 0xd9, count 1) and fetch page 0xd9 (byte 0x37200) via
 *      bus_page_read. If the read succeeded, the buffer is live, the cached page
 *      latched to 0xd9, and the write ends in page 0xd9 ((len + 0x1b3fb) >> 9 ==
 *      0xd9), copy `len` bytes to buf+0x1fc, token the footer page, and tail-call
 *      store_flush (its status is the return). Any sub-step failure -> 1.
 *
 *   B) In-window write (otherwise): reject (return 2) if off, len, or off+len
 *      exceed 0x1b3fc. Else, when the buffer is live and the target span lies
 *      wholly within the currently-cached page (cached == off>>9 ==
 *      (off+len-1)>>9), copy `len` bytes to buf + (off & 0x1ff) and return 0.
 *
 * Returns 0 (in-window write OK / store_flush success in path A), 1 (disabled or
 * NULL store, cache miss, or any path-A sub-step failure), or 2 (span out of
 * range). ABI: f(int h, uint32_t off, const void *src, uint32_t len).
 */
uint32_t fota_image_write(int h, uint32_t off, const void *src, uint32_t len)
{
    store_t *st = (store_t *)(intptr_t)h;
    uint16_t cached;

    if (h == 0 || st->enable == 0) {            /* cbz r0 ; ldrb +0x3c */
        return 1;
    }

    cached = st->page;                          /* ldrh +0x44 */

    if (cached == STORE_PAGE_INVALID && off == FOTA_IMAGE_SIZE) {
        /* Path A: prime + write the image footer page (0xd9 @ 0x37200). */
        st->count = 0;                          /* strh +0x46 = 0 */
        vmem_set(st->buf, 0xff, STORE_PAGE_SIZE);
        /* 32-bit store of 0x100d9 lands page=0xd9 (+0x44), count=1 (+0x46). */
        *(uint32_t *)((uint8_t *)st + 0x44) = FOTA_FOOTER_PAGEIDX;
        if (bus_page_read(st, FOTA_FOOTER_ADDR, st->buf, STORE_PAGE_SIZE) == 0 &&
            st->buf != (uint8_t *)0 &&
            (uint16_t)st->page == STORE_PAGE_LAST &&
            ((len + (FOTA_IMAGE_SIZE - 1)) >> 9) == STORE_PAGE_LAST) {
            /* OEM passes the vmem_copy return (buf+0x1fc) to the token call;
             * r0 is not reloaded (vmem_copy returns dst). // 0x2a6a..0x2a76 */
            if (bus_transfer_token(vmem_copy(st->buf + 0x1fc, src, len),
                                   FOTA_FOOTER_ADDR) == 0) {
                return (uint32_t)store_flush(st);   /* tail-call */
            }
        }
        return 1;
    }

    /* Path B: in-window write. */
    if (off > FOTA_IMAGE_SIZE || len > FOTA_IMAGE_SIZE ||
        off + len > FOTA_IMAGE_SIZE) {          /* bhi (unsigned) */
        return 2;
    }
    if (st->buf != (uint8_t *)0 &&
        cached == (off >> 9) &&
        cached == ((off + len) - 1) >> 9) {
        vmem_copy(st->buf + (off & 0x1ff), src, len);
        return 0;
    }

    return 1;
}

/*
 * fota_image_verify — checksum the staged image region. // 0x00002acc
 *
 * Streams `length` bytes (from byte `offset` within the data region at 0x1c000)
 * one 0x200 page at a time through the HW checksum engine (0x40095000) and
 * publishes the result to *out_crc. Requires the store enabled (+0x3c). Returns
 * 0 ok, 1 (store/flush/read failure), 2 (bad args: NULL out_crc or span past the
 * 0x1b400-byte region). NB: the engine is seeded before the arg/bounds check, so
 * a return-2 leaves it seeded and the power gate enabled (verbatim OEM).
 */
int fota_image_verify(struct store *st, uint32_t offset, uint32_t length, uint32_t *out_crc)
{
    volatile uint32_t *const crc = (volatile uint32_t *)CSUM_BASE; /* DAT_00002b90 */
    uint32_t i;

    if (st == (struct store *)0 || ((store_t *)st)->enable == 0) {
        return 1;
    }

    *(volatile uint32_t *)0x40000220u = 0x200000u;  /* power/clock gate enable */
    port_clock_wait(0x15);
    crc[0] = 0x36u;                                 /* engine control / reset */
    crc[1] = 0xffffffffu;                           /* engine seed            */

    if (out_crc == (uint32_t *)0 || offset + length > FLASH_DATA_SIZE) {
        return 2;
    }
    if (store_flush(st) != 0) {
        return 1;
    }

    {
        store_t *s = (store_t *)st;
        uint32_t addr = offset + FLASH_DATA_BASE;
        for (i = 0; i != (length >> 9); i++) {      /* full 0x200-byte pages */
            int err = bus_page_read(s, addr, s->buf, 0x200);
            addr += 0x200u;
            if (err != 0) {
                return 1;
            }
            checksum_feed(s->buf, 0x200);
        }
        if ((length & 0x1ff) != 0) {                /* trailing partial page */
            if (bus_page_read(s, offset + FLASH_DATA_BASE + i * 0x200u, s->buf, 0x200) != 0) {
                return 1;
            }
            checksum_feed(s->buf, length & 0x1ff);  /* only the remainder fed */
        }

        *out_crc = crc[2];                          /* engine result */
        *(volatile uint32_t *)0x40000240u = 0x200000u; /* power/clock gate disable */
    }
    return 0;
}

/*
 * store_descriptor_read — read the 8-byte storage descriptor @ 0x37400. // 0x00006708
 *
 * Fetches the descriptor via the token path (FUN_0000668c into a 0x200-byte
 * stack scratch buffer) and copies the first 8 bytes out:
 *   - == 0  : descriptor read OK -> vmem_copy(out, scratch, 8).
 *   - == -3 : descriptor page not yet provisioned -> issue the raw vtable page
 *             read (driver+0x10 method); if that itself reports 0 ("nothing
 *             there") the descriptor is treated as all-zero (vmem_set(out,0,8));
 *             otherwise extract the 8 descriptor bytes with FUN_000029b4 and, on
 *             failure, log code 0xa1 (type 3) against DAT_00006790 and return -1.
 *   - other : forwarded as the result (the -3 branch's read result, or the
 *             668c error code) — out left untouched.
 * A NULL session returns -2.
 *
 * Returns the status code; on a 0 result `out` holds the 8-byte descriptor
 * (or zeros when the page was unprovisioned).
 *
 * Disasm anchors (0x6708):
 *   cbz r0,0x6782            -> NULL session, mvn r4,#1 (= -2)
 *   bl 0x668c (r1=sp+8)      -> 668c(sess, scratch)
 *   adds r3,r0,#3 ; beq      -> result == -3 ? raw-read fallback
 *   cbnz r0,0x672e           -> result != 0 (and != -3) -> return result
 *   bl 0x984c (r2=8)         -> vmem_copy(out, scratch, 8)         [result == 0]
 *   ldr r3,[0x678c]=g_bus_mgr; ldr [r3,#0x10]=driver; ldr [.,#0x10]=method
 *   blx r3 (r0=sess,r1=0x37400,r2=0x200) -> driver->page_read(sess,0x37400,0x200)
 *   cbnz r0,0x6758           -> read result != 0 -> extract path
 *   bl 0x9866 (r1=0,r2=8)    -> vmem_set(out, 0, 8)  [read result == 0]
 *   bl 0x29b4 (r1=0x37400,r2=out,r3=8) -> FUN_000029b4(sess,0x37400,out,8)
 *   cbz r0,0x6788            -> extract OK -> return 0
 *   bl 0x3eac (r0=ctx,r1=0xa1,r2=3,r3=0; stack 0x37400, status); r4=-1
 */
int store_descriptor_read(void *sess, void *out)
{
    int status;
    uint8_t scratch[0x200];     /* auStack_210; the token transfer's page buffer */

    if (sess == (void *)0) {            /* cbz r0,0x6782 */
        return -2;                      /* mvn r4,#1 */
    }

    status = flash_page_commit(sess, scratch);   /* 0x668c */
    if (status == -3) {                 /* adds r3,r0,#3 ; beq 0x6736 */
        /* descriptor page not yet provisioned -> raw vtable page load (+0x10) */
        status = g_bus_mgr->driver->page_load(sess, STORE_DESCRIPTOR_ADDR, 0x200u);
        if (status == 0) {                              /* cbnz r0,0x6758 */
            vmem_set(out, 0, 8);                        /* 0x9866(out,0,8) */
            status = 0;
        } else {
            status = bus_page_read(sess, STORE_DESCRIPTOR_ADDR, out, 8); /* 0x29b4 */
            if (status != 0) {                          /* cbz r0,0x6788 */
                event_report(FLASH_EVENT_CTX, 0xa1, 3, 0,
                             STORE_DESCRIPTOR_ADDR, (uint32_t)status); /* 0x3eac */
                status = -1;                            /* mvn r4,#0 */
            }
            /* else: status == 0, out holds the extracted descriptor */
        }
    } else if (status == 0) {           /* cbnz r0,0x672e ; else fallthrough */
        vmem_copy(out, scratch, 8);     /* 0x984c(out, scratch, 8) */
    }
    /* any other status is returned verbatim with `out` left untouched */

    return status;
}

/*
 * store_descriptor_write — write the 8-byte storage descriptor. // 0x00006794
 *
 * Prepares the page via flash_page_commit, builds a zeroed 0x200 page carrying
 * the 8-byte `descriptor`, programs it at the scratch page 0x37a00
 * (flash_page_write), then re-commits. Returns -2 (NULL session), -1 (any
 * prep/program/commit failure), or 0 on success.
 */
int store_descriptor_write(void *sess, const void *descriptor)
{
    unsigned char buf512[516];      /* OEM reserves 0x204, uses only 0x200 */
    int rc;

    if (sess == (void *)0) {                       /* cbz r0 -> -2 */
        return -2;
    }

    rc = flash_page_commit(sess, buf512);          /* 0x668c prep */
    if (rc != -1) {                                /* adds r0,#1 ; bne */
        vmem_set(buf512, 0, 0x200);
        vmem_copy(buf512, descriptor, 8);
        rc = flash_page_write(sess, FLASH_SCRATCH_ADDR, buf512); /* 0x442c @ 0x37a00 */
        if (rc == 0) {
            rc = flash_page_commit(sess, buf512);  /* 0x668c finalize */
            return -(int)(rc != 0);                /* 0 on success, -1 otherwise */
        }
    }
    return -1;
}

/*
 * log_append_event — append an 8-byte event record to the log store. // 0x0000681c
 *
 * When a log-store handle is registered (g_log_store_handle != NULL), build an
 * 8-byte record { src.word0@+0, src.half4@+4, tag=1@+6, flag@+7 } from the
 * runtime source struct and commit it via store_descriptor_write. The caller's
 * `flag` byte is the only argument (the decompiler's extra params are stack
 * artifacts).
 */
void log_append_event(uint8_t flag)
{
    if (g_log_store_handle != (void *)0) {
        struct {
            uint32_t word0;     /* +0x00 */
            uint16_t half4;     /* +0x04 */
            uint8_t  tag;       /* +0x06 */
            uint8_t  flag;      /* +0x07 */
        } record;

        record.word0 = g_log_event_src.word0;      /* ldr  src+0 */
        record.half4 = g_log_event_src.half4;      /* ldrh src+4 */
        record.tag   = 1;                          /* constant tag */
        record.flag  = flag;                       /* caller flag */

        store_descriptor_write(g_log_store_handle, &record);   /* 0x6794 */
    }
}

/*
 * xfer_state_log_notify — transfer-completion handler. // 0x00001884
 *
 * Invoked (via a function pointer) when a transfer/operation `record` completes.
 * It is grouped in this module because its defining VanMoof action is the
 * connection-state log emission via log_append_event; the record itself is the
 * comm/transfer descriptor whose waiter handles live in the SRAM objects above.
 *
 * Steps, in OEM order:
 *   1. If record[+9] bit1 set, reset waiter A; if record[+2] bit1 set, reset B
 *      (xfer_waiter_reset recomputes the waiter's queue + releases its task).
 *   2. Toggle the connection/state flag: code (record[+0xa], u16) == 0x3fd marks
 *      "up" (flag 0->1), anything else marks "down" (flag 1->0); on an actual
 *      transition the new flag is stored and appended to the event log.
 *   3. If record[+0xc] (u16) != 0, hand record+7 to waiter A's queue handle
 *      (waiter_a+0xc) via rtos_sem_give; likewise if record[+5] (u16) != 0, hand
 *      record (base) to waiter B's queue handle (waiter_b+0xc).
 *
 * OEM QUIRK reproduced verbatim: both pre-give "needs reset" tests read waiter
 * A's presence byte (g_xfer_waiter_a[0]) — even the B branch — while the 16-bit
 * field and the reset target are the respective waiter's.
 *
 * The first two args (r0/r1) are ABI placeholders the body never reads; the
 * function returns 0.
 */
uint32_t xfer_state_log_notify(uint32_t a, uint32_t b, const void *record)
{
    const uint8_t *rec = (const uint8_t *)record;
    (void)a;
    (void)b;

    if ((rec[9] & 0x02u) != 0u) {                  /* (byte<<0x1e) < 0 -> bit1 */
        xfer_waiter_reset((void *)g_xfer_waiter_a);
    }
    if ((rec[2] & 0x02u) != 0u) {
        xfer_waiter_reset((void *)g_xfer_waiter_b);
    }

    if (*(const uint16_t *)(rec + 0xa) == 0x3fd) { /* "up" code */
        if (g_xfer_state_flag == 0) {
            g_xfer_state_flag = 1;
            log_append_event(1);
        }
    } else {                                       /* "down" */
        if (g_xfer_state_flag != 0) {
            g_xfer_state_flag = 0;
            log_append_event(0);
        }
    }

    if (*(const uint16_t *)(rec + 0xc) != 0) {
        if (g_xfer_waiter_a[0] != 0 &&
            *(volatile uint16_t *)(g_xfer_waiter_a + 1) == 0) {
            xfer_waiter_reset((void *)g_xfer_waiter_a);
        }
        rtos_sem_give(*(void *volatile *)(g_xfer_waiter_a + 0xc), rec + 7, 1);
    }
    if (*(const uint16_t *)(rec + 5) != 0) {
        if (g_xfer_waiter_a[0] != 0 &&             /* OEM quirk: reads waiter A's byte */
            *(volatile uint16_t *)(g_xfer_waiter_b + 1) == 0) {
            xfer_waiter_reset((void *)g_xfer_waiter_b);
        }
        rtos_sem_give(*(void *volatile *)(g_xfer_waiter_b + 0xc), rec, 1);
    }

    return 0;
}

/*
 * xfer_waiter_notify — reset the waiter queues flagged in a record byte. // 0x00001914
 *
 * Sibling of xfer_state_log_notify; a small leaf reached via a function pointer
 * when a transfer/operation `record` completes. The single flag byte at
 * record[0] selects which waiter(s) to reset: bit1 (0x02) -> waiter A, bit0
 * (0x01) -> waiter B. The first two args (r0/r1) are ABI placeholders the body
 * never reads; returns 0.
 */
uint32_t xfer_waiter_notify(uint32_t a, uint32_t b, const void *record)
{
    const uint8_t *rec = (const uint8_t *)record;
    (void)a;
    (void)b;

    if ((rec[0] & 0x02u) != 0u) {                  /* (byte<<0x1e) < 0 -> bit1 */
        xfer_waiter_reset((void *)g_xfer_waiter_a);
    }
    if ((rec[0] & 0x01u) != 0u) {                  /* (byte<<0x1f) < 0 -> bit0 */
        xfer_waiter_reset((void *)g_xfer_waiter_b);
    }

    return 0;
}

/*
 * xfer_waiter_post_frame — assemble a 7-byte completion frame and post it to the
 * two transfer waiters. // 0x0000193c
 *
 * Two near-identical blocks, one per waiter object (A @ 0x200006b4, B @
 * 0x20000970). Each loads the waiter's embedded queue/sem handle (waiter+0xc;
 * NULL -> configASSERT trap), samples handle+0x38 under a critical section, and
 * posts the frame iff the queue is idle (handle+0x38 == 0), the skip-lock
 * (waiter+0x03 bit0) is clear and the presence byte (waiter+0x00) is zero. The
 * 7-byte frame: [0..1]=1, [2]=(arg0>>16)&0xfe, [3..4]=len_a+len_b-1, [5]=1,
 * [6]=0. arg1 is an unused placeholder; arg2's first byte is latched at
 * 0x2000001c. Returns 0. (OEM `bfi r3,r1,#0,#1` clears byte[2] bit0 with the
 * presence byte, provably 0 on the post path -> modeled as `&= 0xfe`.)
 */
typedef struct xfer_waiter {
    uint8_t  present;       /* +0x00 presence; must be 0 to post           */
    uint8_t  _pad01[2];     /* +0x01..+0x02                                 */
    uint8_t  lock;          /* +0x03 bit0: skip-post lock                   */
    uint16_t len_a;         /* +0x04 length field A                         */
    uint16_t len_b;         /* +0x06 length field B                         */
    uint8_t  _pad08[4];     /* +0x08..+0x0b                                 */
    void    *handle;        /* +0x0c embedded queue/sem handle             */
} xfer_waiter_t;

/* The 7-byte framed message posted to each waiter's queue. */
typedef struct xfer_frame {
    uint16_t _h0;           /* +0x00 = 1                                    */
    uint8_t  type;          /* +0x02 = (arg0>>16) & 0xfe                    */
    uint16_t length;        /* +0x03 = len_a + len_b - 1                    */
    uint8_t  flag;          /* +0x05 = 1                                    */
    uint8_t  tail;          /* +0x06 = 0                                    */
} __attribute__((packed)) xfer_frame_t;

/* Byte latched from the posted record (DAT_000019f0). */
#define g_xfer_post_byte    (*(volatile uint8_t *)0x2000001cu)

uint32_t xfer_waiter_post_frame(uint32_t arg0, uint32_t arg1, const uint8_t *arg2)
{
    xfer_frame_t   frame;
    xfer_waiter_t *w;
    int            blocked;

    (void)arg1;

    frame.type = (uint8_t)(arg0 >> 16);     /* sp+2 seed: arg0 byte2     */
    g_xfer_post_byte = *arg2;               /* *0x2000001c = *arg2       */

    /* ---- waiter A @ 0x200006b4 ---- */
    w = (xfer_waiter_t *)g_xfer_waiter_a;
    if (w->handle == 0) {                    /* configASSERT(handle)     */
        vPortRaiseBASEPRI();
        for (;;) { }
    }
    vPortEnterCritical();
    blocked = *(int volatile *)((uint8_t *)w->handle + 0x38);
    vPortExitCritical();
    if (blocked == 0 && (w->lock & 1u) == 0u && w->present == 0u) {
        frame.type  &= 0xfeu;                              /* bfi bit0 <- 0 */
        frame.length = (uint16_t)(w->len_b + w->len_a - 1);
        frame.flag   = 1;
        frame._h0    = 1;
        frame.tail   = 0;
        rtos_sem_give(w->handle, &frame, 1);               /* 0x6ec0 */
    }

    /* ---- waiter B @ 0x20000970 ---- */
    w = (xfer_waiter_t *)g_xfer_waiter_b;
    if (w->handle == 0) {                    /* configASSERT(handle)     */
        vPortRaiseBASEPRI();
        for (;;) { }
    }
    vPortEnterCritical();
    blocked = *(int volatile *)((uint8_t *)w->handle + 0x38);
    vPortExitCritical();
    if (blocked == 0 && (w->lock & 1u) == 0u && w->present == 0u) {
        frame.type  &= 0xfeu;
        frame.length = (uint16_t)(w->len_b + w->len_a - 1);
        frame.flag   = 1;
        frame._h0    = 1;
        frame.tail   = 0;
        rtos_sem_give(w->handle, &frame, 1);               /* 0x6ec0 */
    }

    return 0;
}

/*
 * timer_remaining_ticks — ticks remaining until a software timer expires.
 * // 0x000019f8
 *
 * `timer_holder` is the pointer the OEM passes in r0; *timer_holder is a
 * FreeRTOS software-timer object (allocated by FUN_000015e0 with
 * pvPortMalloc(0x28)). The object carries its active flag at +0x24 (bit0) and
 * its expiry tick at +0x04. Returns 0 when the holder/timer is NULL or the
 * timer is inactive, else (expiry - xTickCount) — the signed ticks-until-fire.
 * The status byte is sampled inside a critical section; the post-critical
 * re-read with a configASSERT trap (vPortRaiseBASEPRI + spin) is reproduced.
 * Callers: device_mgr_reset (0x3f86), button_scan_poll (0x3e1c).
 */
int timer_remaining_ticks(void **timer_holder)
{
    void    *timer;
    uint8_t  status;

    if (timer_holder == (void **)0) {           /* cbnz r0 ; else return 0 */
        return 0;
    }
    timer = *timer_holder;                       /* ldr r5,[r0]            */
    if (timer == (void *)0) {                    /* cmp r5,#0 ; beq        */
        return 0;
    }

    vPortEnterCritical();                        /* bl 0x6454              */
    status = *(volatile uint8_t *)((uint8_t *)timer + 0x24) & 0x01u; /* ldrb +0x24 ; and #1 */
    vPortExitCritical();                         /* bl 0x6470              */

    if (status == 0) {                           /* timer not active       */
        return 0;
    }

    timer = *timer_holder;                       /* ldr r3,[r4]  (re-read) */
    if (timer == (void *)0) {                    /* cbnz r3 ; else panic   */
        vPortRaiseBASEPRI();                     /* configASSERT fail path */
        for (;;) {
        }
    }

    /* (expiry tick @ +0x04) - xTickCount @ 0x20001e44 */
    return *(int *)((uint8_t *)timer + 0x04)
         - *(volatile int *)0x20001e44u;
}
