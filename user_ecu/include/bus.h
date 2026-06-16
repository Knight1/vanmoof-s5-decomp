#ifndef USER_ECU_BUS_H
#define USER_ECU_BUS_H

#include <stdint.h>

/*
 * bus.h — polymorphic I²C/bus session layer.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * A transient "session" object (0x3c bytes, heap-allocated per transaction) is
 * driven through a single global driver manager at the fixed address
 * 0x1301fe00. The manager's +0x10 field points at a driver vtable; every bus
 * primitive selects one of two method variants (A/B) by a board/bus selector
 * byte at manager+0x06 (== 3 -> variant B). The variant methods sit 0xc apart:
 *
 *   init   +0x04          open  +0x1c / +0x28
 *   write  +0x24 / +0x30  xfer  +0x2c / +0x38   probe +0x40 / +0x4c
 *
 * The concrete vtable targets are populated at init (device-registration sweep
 * in main_SystemInit) and are not statically resolvable here.
 */

struct bus_session;

/*
 * Driver vtable object (manager+0x10). Only the dispatched slots are typed; the
 * gaps are reproduced as opaque words so the byte offsets match the OEM.
 */
typedef struct bus_driver {
    void *_w00;                                                              /* +0x00 */
    int (*init)(struct bus_session *sess);                                   /* +0x04 */
    int (*xfer_token)(void *a, uint32_t b, uint32_t len, uint32_t token);    /* +0x08 (bus_transfer_token, variant B) */
    int (*page_program)(void *sess, uint32_t addr, void *buf, uint32_t len); /* +0x0c (bus_page_program, variant B) */
    void *_w10;                                                              /* +0x10 */
    void *_w14;                                                              /* +0x14 */
    void *_w18;                                                              /* +0x18 */
    int (*open_a)(struct bus_session *sess);                                 /* +0x1c */
    void *_w20;                                                              /* +0x20 */
    int (*write_a)(struct bus_session *sess, void *desc, int z);             /* +0x24 */
    int (*open_b)(struct bus_session *sess);                                 /* +0x28 */
    int (*xfer_a)(struct bus_session *sess, void *buf, uint32_t addr, uint32_t len); /* +0x2c */
    int (*write_b)(struct bus_session *sess, void *desc, int z);             /* +0x30 */
    void *_w34;                                                              /* +0x34 */
    int (*xfer_b)(struct bus_session *sess, void *buf, uint32_t addr, uint32_t len); /* +0x38 */
    void *_w3c;                                                              /* +0x3c */
    int (*probe_a)(struct bus_session *sess, void *buf, int z, uint32_t n);  /* +0x40 */
    void *_w44;                                                              /* +0x44 */
    void *_w48;                                                              /* +0x48 */
    int (*probe_b)(struct bus_session *sess, void *buf, int z, uint32_t n);  /* +0x4c */
} bus_driver_t;

/* Global driver manager (fixed address; variant byte at +0x06, driver at +0x10). */
typedef struct bus_mgr {
    uint8_t       _b00[6];      /* +0x00 */
    uint8_t       variant;      /* +0x06 board/bus selector (== 3 -> variant B) */
    uint8_t       _b07[9];      /* +0x07..+0x0f */
    bus_driver_t *driver;       /* +0x10 driver vtable object */
} bus_mgr_t;

#define g_bus_mgr  ((bus_mgr_t *)0x1301fe00u)

/*
 * Per-transaction session (0x3c bytes). Only the OEM-touched fields are named;
 * the driver's init/open methods populate the rest.
 */
typedef struct bus_session {
    uint32_t _w00;          /* +0x00 */
    uint32_t timing;        /* +0x04 timing/divider (0xa0000 sentinel -> recompute) */
    uint32_t _w08;          /* +0x08 */
    uint32_t param;         /* +0x0c divider input (timing = 0xa0000 - 17*param) */
    uint32_t _w10[4];       /* +0x10..+0x1c */
    uint32_t speed;         /* +0x20 autodetected metric (max probe value) */
    uint32_t mode;          /* +0x24 autodetected mode (1 or 2) */
    uint32_t state;         /* +0x28 set to 0x60 by bus_session_init */
    uint32_t _w2c[4];       /* +0x2c..+0x38 (total 0x3c) */
} bus_session_t;

/* --- variant selection ---------------------------------------------------- */

/* True when the board/bus selector chooses the "B" driver methods. // 0x0000288c */
int bus_variant_b(void);

/* --- driver-method dispatch shims ----------------------------------------- */

/* Read/transfer `len` bytes at sub-address `addr` into `buf`. // 0x00002910 */
int bus_transfer(bus_session_t *sess, void *buf, uint32_t addr, uint32_t len);

/* Commit a descriptor/page write. // 0x00006538 */
int bus_write_commit(bus_session_t *sess, void *desc);

/* Probe-read a 0x200-byte page into `buf` (used by mode autodetect). // 0x000065dc */
int bus_probe_read(bus_session_t *sess, void *buf);

/* --- session lifecycle ---------------------------------------------------- */

/* Initialise a freshly allocated session; returns the driver init status. // 0x0000289c */
int bus_session_init(bus_session_t *sess);

/* Probe modes 1 and 2 and latch the one with the larger metric. // 0x000089c0 */
int bus_mode_autodetect(bus_session_t *sess);

/*
 * Allocate + initialise + open + autodetect a session. Returns the session on
 * success or NULL (freeing on any failure). // 0x000028c8
 */
bus_session_t *bus_session_open(void);

/*
 * Read-modify-write the device's 0x200 config page: when `set` is non-zero, OR
 * in bits 0x70 and stamp the sentinel {0xff2000df, 0xffff0000} at +0x10/+0x14;
 * otherwise clear +0x10/+0x14. Writes the page back only if it changed. Returns
 * 0 on success, non-zero on failure (incl. NULL session / OOM). // 0x00002938
 */
int bus_session_commit(bus_session_t *sess, int set);

/*
 * bus_page_write_verify — splice `len` bytes of `buf` into the device's 0x200
 * config page at byte offset 0x30 + `off`, write the page back, re-read just
 * that region (sub-address (off + 0x30) & 0xff) and memcmp it against `buf`.
 * Returns 0 on a verified write; 1 on OOM, read/write failure, NULL session, or
 * a read-back mismatch. // 0x00008b12
 */
int bus_page_write_verify(bus_session_t *sess, const void *buf, uint32_t len, uint32_t off);

/*
 * bus_transfer_token — variant-dispatched 0x200-byte bus op carrying a fixed
 * token. Variant A calls a fixed off-image handler (0x1300413a); variant B calls
 * the driver vtable method at +0x08. Both receive (a, b, 0x200, 0x6b65666c) and
 * the OEM tail-calls, forwarding the result. The precise operation is
 * unconfirmed (the variant-A handler and the manager live in the off-image
 * 0x13xxxxxx region); only the dispatch is reconstructed here. // 0x0000664c
 */
int bus_transfer_token(void *a, uint32_t b);

/*
 * bus_page_program — program a 0x200-byte page at byte address `addr` from
 * `buf`. Variant A calls a fixed off-image handler (0x1300419c); variant B calls
 * the driver vtable method at +0x0c. Both receive (sess, addr, buf, 0x200). Used
 * by the storage page-cache write-back. // 0x00006610
 */
int bus_page_program(void *sess, uint32_t addr, void *buf);

#endif /* USER_ECU_BUS_H */
