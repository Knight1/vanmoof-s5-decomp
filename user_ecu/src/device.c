/*
 * device.c — registry-backed device-record cache accessors.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions:
 *   device_read_record87  @ 0x00008e76  (read 14-byte record, tag {id,0x87})
 *   device_store_field8c0 @ 0x00008e0a  (write 3-byte field, tag {0xc0,0x08})
 *   device_cmd_read87     @ 0x00008f76  (command write+verify, then read87)
 *   device_read_record91  @ 0x00008fd6  (read 16-byte record, tag {id,0x91})
 *
 * Each accessor opens a transient I²C bus session, transfers a record to/from
 * the part, and refreshes the registry-cached copy under the device semaphore.
 * The registry is the device table reconstructed in registry.c; the slot
 * returned by registry_lookup_value is a 0x2c-byte record whose first two words
 * are the cached buffer pointer and the access semaphore (see dev_record).
 */

#include <stdint.h>

#include "device.h"
#include "registry.h"
#include "util.h"       /* vmem_set, vmem_copy, vmem_cmp */

/*
 * A registry device slot (0x2c bytes). Only the first two words are touched by
 * these accessors; the registry's lookup key lives at +0xc (REGISTRY_KEY_OFFSET).
 */
typedef struct dev_record {
    uint8_t *data;      /* +0x00 cached record buffer */
    void    *sem;       /* +0x04 access semaphore (FreeRTOS) */
} dev_record_t;

/*
 * VanMoof I²C bus-session glue — not yet translated; declared here in
 * decompiler form (ABI as used at the call sites). Future decomp targets.
 */
extern void *FUN_000028c8(void);                                        /* open session -> handle/NULL */
extern int   FUN_00002910(void *sess, void *buf, uint32_t addr, uint32_t len); /* bus transfer */
extern int   FUN_00002938(void *sess, int flush);                       /* session commit/flush */
extern void  FUN_000087f2(void *sess);                                  /* close (free-if-non-NULL) */
extern int   FUN_00008656(registry_t *mgr, void *rec);                  /* apply/notify hook */
extern int   FUN_00008b12(void *sess, const void *buf, uint32_t len, int off); /* write + read-back verify */

/* FreeRTOS — vendor (deferred). */
extern int   xQueueSemaphoreTake(void *sem, uint32_t ticks);            /* 0x00008d54 */
extern void  rtos_sem_give_dispatch(void *sem);                                   /* semaphore give (0x000097f4) */

/* Device-class lookup tags (low 3 bytes of registry_lookup_value's key word). */
#define DEV_TAG_TYPE87    0x87u
#define DEV_TAG_TYPE91    0x91u
#define DEV_TAG_8C0       0x08c0u

/*
 * device_read_record87 — refresh device {id, 0x87} from the bus. // 0x00008e76
 *
 * Reads 14 bytes from sub-address 0x30, rejects a leading length/status byte
 * > 1, then caches the 13-byte payload (bytes 1..13) into the device record
 * under its semaphore. With `expect`, the raw 14-byte read is memcmp'd against
 * it. Returns 0 on success, -1 otherwise.
 */
int device_read_record87(dev_handle_t *dev, const void *expect)
{
    uint8_t       buf[14];
    int           read_status;
    int           copy_status;
    int           ret;
    void         *session;
    dev_record_t *rec;
    registry_t   *reg;
    uint8_t       id;

    session = FUN_000028c8();                       /* open bus session */
    vmem_set(buf, 0, 0xe);
    if (session == (void *)0) {
        read_status = -1;
    } else {
        read_status = -(FUN_00002910(session, buf, 0x30, 0xe) != 0);
    }
    if (1 < buf[0]) {                               /* invalid length/status byte */
        vmem_set(buf, 0, 0xe);
        read_status = -1;
    }

    id  = dev->id;
    reg = dev->reg;

    /* Lookup tag {id, 0x87, 0}. registry_lookup_value re-stacks the key; only
     * the low 3 bytes are compared, so the 2nd key word is a don't-care. */
    rec = (dev_record_t *)registry_lookup_value(reg,
              (uint32_t)id | (DEV_TAG_TYPE87 << 8), 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        int i;
        /* cache the 13-byte payload (buf[1..13]); OEM inlines a 3-word+1-byte copy */
        for (i = 0; i < 13; i++) {
            data[i] = buf[1 + i];
        }
        copy_status = 0;
        rec = (dev_record_t *)registry_lookup_value(reg,
                  (uint32_t)id | (DEV_TAG_TYPE87 << 8), 0);
        if (rec != (dev_record_t *)0) {
            rtos_sem_give_dispatch(rec->sem);                 /* release the access semaphore */
        } else {
            copy_status = -1;
        }
    } else {
        copy_status = -1;
    }

    if (read_status != 0) {
        copy_status = -1;
    }
    if (session == (void *)0) {
        ret = -1;
    } else {
        FUN_000087f2(session);                      /* close session */
        ret = 0;
    }
    if (copy_status != 0) {
        ret = -1;
    }
    if (expect != (const void *)0) {
        int diff = vmem_cmp(buf, expect, 0xe);
        if (ret == 0) {
            ret = -(diff != 0);
        } else {
            ret = -1;
        }
    }
    return ret;
}

/*
 * device_store_field8c0 — write a 3-byte field into device {0xc0,0x08}. // 0x00008e0a
 *
 * Looks up the device, takes its semaphore, stores src[0..2] into the cached
 * record, runs the apply/notify hook and releases the semaphore (OEM tail-calls
 * the give). `arg2` mirrors the OEM's unused 3rd register argument.
 */
void device_store_field8c0(registry_t *reg, const void *src, uint32_t arg2)
{
    const uint8_t *s = (const uint8_t *)src;
    dev_record_t  *rec;

    (void)arg2;     /* OEM leaves r2 (the 2nd key word) as the caller's; unused */

    rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8C0, 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        /* store the 3-byte field (OEM: strh src[0..1] + strb src[2]) */
        data[0] = s[0];
        data[1] = s[1];
        data[2] = s[2];
        rec = (dev_record_t *)registry_lookup_value(reg, DEV_TAG_8C0, 0);
        if (rec != (dev_record_t *)0) {
            FUN_00008656(reg, rec);                 /* apply/notify hook */
            rtos_sem_give_dispatch(rec->sem);                 /* release semaphore (tail call) */
        }
    }
}

/*
 * device_cmd_read87 — command write-then-read of device {dev->id, 0x87}. // 0x00008f76
 *
 * Builds a 14-byte frame (0x01 || payload[13]), writes it with read-back verify
 * (FUN_00008b12), commits the session (FUN_00002938), then refreshes the device
 * via device_read_record87 with the frame as the expected value. `arg2` mirrors
 * the OEM's unused 2nd register argument. Returns the read result, or -1 on any
 * session/write failure.
 */
int device_cmd_read87(dev_handle_t *dev, uint32_t arg2, const void *payload)
{
    uint8_t  frame[14];
    void    *session;
    int      write_status;
    int      commit_status;
    int      read_result;

    (void)arg2;     /* OEM never reads r1 (the 2nd argument) */

    session = FUN_000028c8();
    if (session != (void *)0) {
        frame[0] = 1;
        vmem_copy(&frame[1], payload, 0xd);                  /* 13-byte payload */
        write_status  = FUN_00008b12(session, frame, 0xe, 0);/* write + verify */
        commit_status = FUN_00002938(session, 1);            /* commit/flush */
        if (write_status == 0) {
            write_status = -(commit_status != 0);
        } else {
            write_status = -1;
        }
        FUN_000087f2(session);                               /* close session */
        read_result = device_read_record87(dev, frame);
        if (write_status == 0) {
            return read_result;
        }
    }
    return -1;
}

/*
 * device_read_record91 — refresh device {id, 0x91} from the bus. // 0x00008fd6
 *
 * Reads 16 bytes from sub-address 0x40 and caches the full 16-byte record into
 * the device record under its semaphore (no length-byte gate, unlike the 0x87
 * reader). With `expect`, the raw 16-byte read is memcmp'd against it. Returns 0
 * on success, -1 otherwise.
 */
int device_read_record91(dev_handle_t *dev, const void *expect)
{
    uint8_t       buf[16];
    int           read_status;
    int           copy_status;
    int           ret;
    void         *session;
    dev_record_t *rec;
    registry_t   *reg;
    uint8_t       id;

    session = FUN_000028c8();
    vmem_set(buf, 0, 0x10);
    if (session == (void *)0) {
        read_status = -1;
    } else {
        read_status = -(FUN_00002910(session, buf, 0x40, 0x10) != 0);
    }

    id  = dev->id;
    reg = dev->reg;

    rec = (dev_record_t *)registry_lookup_value(reg,
              (uint32_t)id | (DEV_TAG_TYPE91 << 8), 0);
    if (rec != (dev_record_t *)0 &&
        xQueueSemaphoreTake(rec->sem, 100) == 0) {
        uint8_t *data = rec->data;
        int i;
        /* cache all 16 bytes; OEM inlines a 2-words-per-iteration copy */
        for (i = 0; i < 16; i++) {
            data[i] = buf[i];
        }
        copy_status = 0;
        rec = (dev_record_t *)registry_lookup_value(reg,
                  (uint32_t)id | (DEV_TAG_TYPE91 << 8), 0);
        if (rec != (dev_record_t *)0) {
            rtos_sem_give_dispatch(rec->sem);
        } else {
            copy_status = -1;
        }
    } else {
        copy_status = -1;
    }

    if (read_status != 0) {
        copy_status = -1;
    }
    if (session == (void *)0) {
        ret = -1;
    } else {
        FUN_000087f2(session);
        ret = 0;
    }
    if (copy_status != 0) {
        ret = -1;
    }
    if (expect != (const void *)0) {
        int diff = vmem_cmp(buf, expect, 0x10);
        if (ret == 0) {
            ret = -(diff != 0);
        } else {
            ret = -1;
        }
    }
    return ret;
}
