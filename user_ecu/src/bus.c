/*
 * bus.c — polymorphic I²C/bus session layer.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   bus_variant_b        @ 0x0000288c  (board/bus selector check)
 *   bus_transfer         @ 0x00002910  (read/xfer dispatch, vtable +0x2c/+0x38)
 *   bus_write_commit     @ 0x00006538  (write dispatch, vtable +0x24/+0x30)
 *   bus_probe_read       @ 0x000065dc  (probe read, vtable +0x40/+0x4c)
 *   bus_session_init     @ 0x0000289c  (init + timing recompute)
 *   bus_mode_autodetect  @ 0x000089c0  (latch faster of modes 1/2)
 *   bus_session_open     @ 0x000028c8  (alloc + init + open + autodetect)
 *   bus_session_commit   @ 0x00002938  (0x200-page read-modify-write)
 *   bus_page_write_verify@ 0x00008b12  (splice + write-back + read-back verify)
 *
 * Every transfer primitive dispatches through the global driver manager
 * (g_bus_mgr, fixed at 0x1301fe00): manager+0x10 is the driver vtable and the
 * A/B method variant is chosen by bus_variant_b().
 */

#include <stddef.h>
#include <stdint.h>

#include "bus.h"
#include "util.h"      /* mem_free */

/* FreeRTOS heap_4 allocator (vendor, deferred) — 0x00006a10. */
extern void *pvPortMalloc(size_t xWantedSize);

/* Config-page sentinel written by bus_session_commit (DAT_000029b0/000029ac). */
#define BUS_PAGE_MARK_LO  0xff2000dfu
#define BUS_PAGE_MARK_HI  0xffff0000u

/*
 * bus_variant_b — board/bus selector test. // 0x0000288c
 *
 * OEM: return *(char *)(g_bus_mgr + 6) == 3. Selects the "B" set of driver
 * vtable methods (each 0xc above its "A" sibling) when true.
 */
int bus_variant_b(void)
{
    return g_bus_mgr->variant == 3;
}

/*
 * bus_transfer — read/transfer dispatch. // 0x00002910
 *
 * Tail-dispatches to the driver's transfer method (vtable +0x2c for variant A,
 * +0x38 for B), forwarding (sess, buf, addr, len). Returns the method's status.
 */
int bus_transfer(bus_session_t *sess, void *buf, uint32_t addr, uint32_t len)
{
    bus_driver_t *drv = g_bus_mgr->driver;

    if (bus_variant_b() == 0) {
        return drv->xfer_a(sess, buf, addr, len);
    }
    return drv->xfer_b(sess, buf, addr, len);
}

/*
 * bus_write_commit — descriptor/page write dispatch. // 0x00006538
 *
 * Tail-dispatches to the driver's write method (vtable +0x24 / +0x30) with a
 * trailing zero argument. Returns the method's status.
 */
int bus_write_commit(bus_session_t *sess, void *desc)
{
    bus_driver_t *drv = g_bus_mgr->driver;

    if (bus_variant_b() == 0) {
        return drv->write_a(sess, desc, 0);
    }
    return drv->write_b(sess, desc, 0);
}

/*
 * bus_probe_read — 0x200-byte probe read dispatch. // 0x000065dc
 *
 * Tail-dispatches to the driver's probe method (vtable +0x40 / +0x4c) with
 * (sess, buf, 0, 0x200). Returns the method's status.
 */
int bus_probe_read(bus_session_t *sess, void *buf)
{
    bus_driver_t *drv = g_bus_mgr->driver;

    if (bus_variant_b() == 0) {
        return drv->probe_a(sess, buf, 0, 0x200);
    }
    return drv->probe_b(sess, buf, 0, 0x200);
}

/*
 * bus_session_init — initialise a freshly allocated session. // 0x0000289c
 *
 * Stamps state (+0x28) = 0x60, runs the driver init method (vtable +0x04), then
 * — if the driver left the timing field (+0x04) at the 0xa0000 sentinel —
 * recomputes it as 0xa0000 - 17*param (param at +0x0c; OEM `r + (r << 4)` = 17r,
 * then `rsb #0xa0000`). Returns the driver init status (r0).
 */
int bus_session_init(bus_session_t *sess)
{
    int rc;

    sess->state = 0x60;                         /* str #0x60,[sess,#0x28] */
    rc = g_bus_mgr->driver->init(sess);         /* blx vtable+0x04 */
    if (sess->timing == 0xa0000) {              /* cmp #0xa0000 ; it(ttt) eq */
        sess->timing = 0xa0000u - 0x11u * sess->param;
    }
    return rc;
}

/*
 * bus_mode_autodetect — pick the faster of modes 1 and 2. // 0x000089c0
 *
 * Probe-reads a 0x200-byte page in mode 1 then mode 2 (mode at +0x24), reading
 * a metric from page word 1 (offset +4) each time, and latches {metric, mode}
 * at +0x20/+0x24 for whichever mode yielded the larger metric. Returns the last
 * probe status (0 only if both probes succeeded; the caller frees the session
 * unless this is 0).
 */
int bus_mode_autodetect(bus_session_t *sess)
{
    uint32_t page[0x200 / sizeof(uint32_t)];
    uint32_t metric1;
    int rc;

    sess->mode = 1;                             /* str #1,[sess,#0x24] */
    rc = bus_probe_read(sess, page);
    if (rc == 0) {
        metric1 = page[1];                      /* ldr [sp,#4] */
        sess->mode = 2;
        rc = bus_probe_read(sess, page);
        if (rc == 0) {
            uint32_t metric2 = page[1];
            if (metric2 < metric1) {            /* cmp hi -> mode 1 wins */
                sess->speed = metric1;
                sess->mode = 1;
            } else {
                sess->speed = metric2;
                sess->mode = 2;
            }
        }
    }
    return rc;
}

/*
 * bus_session_open — allocate, initialise, open and autodetect a session. // 0x000028c8
 *
 * Allocates a 0x3c-byte session, runs bus_session_init; on success opens the
 * driver (vtable +0x1c for variant A, +0x28 for B) and runs mode autodetect.
 * Returns the live session, or NULL after freeing it on any failure.
 */
bus_session_t *bus_session_open(void)
{
    bus_session_t *sess;

    sess = (bus_session_t *)pvPortMalloc(0x3c);
    if (sess == (bus_session_t *)0) {
        return (bus_session_t *)0;
    }

    if (bus_session_init(sess) == 0) {          /* cbz r0 -> continue */
        bus_driver_t *drv = g_bus_mgr->driver;
        int (*open)(bus_session_t *) =
            bus_variant_b() ? drv->open_b : drv->open_a;
        if (open(sess) == 0 && bus_mode_autodetect(sess) == 0) {
            return sess;
        }
    }

    mem_free(sess);                             /* 0x000087f2 */
    return (bus_session_t *)0;
}

/*
 * bus_session_commit — read-modify-write the 0x200-byte config page. // 0x00002938
 *
 * Reads the page (sub-address 0), edits the flag/sentinel fields per `set`, and
 * writes it back only when something changed. `set` != 0 enables (OR 0x70 into
 * word 0, stamp the {LO,HI} sentinel at words 4/5); `set` == 0 clears words 4/5.
 * Returns 0 on success (incl. "no change"), non-zero on NULL session, OOM, read
 * or write-back failure.
 */
int bus_session_commit(bus_session_t *sess, int set)
{
    uint32_t *page;
    uint32_t  old10;
    uint32_t  old14;
    int       rc;

    if (sess == (bus_session_t *)0) {
        return 1;
    }
    page = (uint32_t *)pvPortMalloc(0x200);
    if (page == (uint32_t *)0) {
        return 1;
    }

    rc = bus_transfer(sess, page, 0, 0x200);    /* read current page */
    if (rc != 0) {
        goto out;
    }

    old10 = page[4];                            /* page+0x10 */
    old14 = page[5];                            /* page+0x14 */
    if (set == 0) {
        page[4] = 0;
        page[5] = 0;
        if (old10 == page[4] && old14 == page[5]) {
            goto out;                           /* unchanged -> skip write */
        }
    } else {
        uint32_t old0 = page[0];
        uint32_t new0 = old0 | 0x70u;
        page[4] = BUS_PAGE_MARK_LO;
        page[5] = BUS_PAGE_MARK_HI;
        page[0] = new0;
        if (old0 == new0 && old10 == page[4] && old14 == page[5]) {
            goto out;                           /* nothing changed -> skip write */
        }
    }
    rc = bus_write_commit(sess, page);

out:
    mem_free(page);
    return rc != 0;
}

/*
 * bus_page_write_verify — register write with read-back verify. // 0x00008b12
 *
 * Reads the device's 0x200 config page, copies `len` bytes of `buf` into it at
 * page offset 0x30 + `off`, writes the whole page back, then re-reads just that
 * region (sub-address (off + 0x30) & 0xff, `len` bytes) and compares it to
 * `buf`. Returns 0 only when the write verified; 1 on OOM, the initial read,
 * the write-back, the NULL-session guard, the re-read, or a vmem_cmp mismatch.
 */
int bus_page_write_verify(bus_session_t *sess, const void *buf, uint32_t len, uint32_t off)
{
    uint8_t *page;
    int      rc;

    page = (uint8_t *)pvPortMalloc(0x200);
    if (page == (uint8_t *)0) {
        return 1;                               /* OOM */
    }

    if (bus_transfer(sess, page, 0, 0x200) == 0) {
        uint8_t *target = page + 0x30 + off;
        vmem_copy(target, buf, len);            /* splice the new bytes in */
        if (bus_write_commit(sess, page) == 0 &&
            sess != (bus_session_t *)0 &&
            bus_transfer(sess, target, (off + 0x30) & 0xff, len) == 0) {
            rc = (vmem_cmp(target, buf, len) != 0);   /* 0 = verified */
        } else {
            rc = 1;
        }
    } else {
        rc = 1;
    }

    mem_free(page);
    return rc;
}
