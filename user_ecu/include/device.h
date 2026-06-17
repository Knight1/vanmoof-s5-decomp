#ifndef USER_ECU_DEVICE_H
#define USER_ECU_DEVICE_H

#include <stdint.h>

#include "registry.h"

/*
 * device.h — registry-backed device-record cache accessors.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * A "device" is a fixed 0x2c-byte registry slot (dev_record) keyed by a 3-byte
 * tag {id, type, 0} at +0x0c. The slot caches the part's record buffer (+0x00),
 * an access semaphore (+0x04), and the descriptor fields the apply hook
 * transmits. These accessors open a transient I²C bus session (bus_session_open),
 * transfer a record to/from the part, and refresh the cached copy under the
 * semaphore.
 *
 * The 16-bit lookup tags seen here are device-class selectors:
 *   0x87  — 14-byte record (sub-address 0x30)   [device_read_record87]
 *   0x91  — 16-byte record (sub-address 0x40)   [device_read_record91]
 *   0x08c0 — 3-byte field write/apply           [device_store_field8c0]
 */

/*
 * dev_record — a device's 0x2c-byte registry slot. The reader accessors touch
 * only data/sem; the apply hook (device_apply) also reads the descriptor
 * (length, key, type, aux) to build the transmit frame.
 */
typedef struct dev_record {
    uint8_t  *data;     /* +0x00 cached record buffer                          */
    void     *sem;      /* +0x04 access semaphore (FreeRTOS)                    */
    uint32_t  length;   /* +0x08 payload length (bytes to transmit)            */
    uint32_t  key;      /* +0x0c lookup tag {id, type, 0} in the low 3 bytes   */
    uint8_t   type;     /* +0x10 transmit mode: 0 single-shot, 1/2 chunked     */
    uint8_t   _b11[3];  /* +0x11..+0x13                                        */
    uint32_t  _w14;     /* +0x14                                               */
    uint32_t  _w18;     /* +0x18                                               */
    uint32_t  _w1c;     /* +0x1c                                               */
    void     *aux;      /* +0x20 aux payload pointer (type 0)                  */
    uint32_t  aux_len;  /* +0x24 aux payload length                            */
    uint32_t  _w28;     /* +0x28  (slot is 0x2c bytes total)                   */
} dev_record_t;

/*
 * dev_handle — {registry, id} pair identifying one device + sub-id. The reader
 * accessors form their lookup tag from `id` (low byte) and a fixed type byte.
 * Only fields +0x00 and +0x04 are touched by the OEM.
 */
typedef struct dev_handle {
    registry_t *reg;    /* +0x00 device registry */
    uint8_t     id;     /* +0x04 device sub-id (low byte of the lookup tag) */
} dev_handle_t;

/*
 * device_read_record87 — refresh device {id, 0x87}'s 14-byte cached record from
 * the bus (sub-address 0x30) under its semaphore. When `expect` is non-NULL the
 * freshly read 14 bytes are memcmp'd against it. Returns 0 on success (and, with
 * `expect`, a match), -1 otherwise. // 0x00008e76
 */
int device_read_record87(dev_handle_t *dev, const void *expect);

/*
 * device_read_record91 — refresh device {id, 0x91}'s 16-byte cached record from
 * the bus (sub-address 0x40) under its semaphore; optional `expect` compare as
 * above. Returns 0 / -1. // 0x00008fd6
 */
int device_read_record91(dev_handle_t *dev, const void *expect);

/*
 * device_store_field8c0 — write the 3-byte field `src` into device {0xc0,0x08}'s
 * cached record under its semaphore, then run the apply/notify hook
 * (device_apply) and release the semaphore. `arg2` mirrors the OEM's unused 3rd
 * register argument. // 0x00008e0a
 */
void device_store_field8c0(registry_t *reg, const void *src, uint32_t arg2);

/*
 * device_apply — build a command frame from `rec`'s descriptor and transmit it
 * through the device manager's channel (`mgr`+0x594, send method at +0x5a4).
 * The frame carries the record key (rec+0x0c); `rec->type` selects single-shot
 * transmit (type 0, optionally appending the rec->aux payload) or chunked
 * transmit (types 1/2, ≤8 bytes per frame with a wrapping sequence counter).
 * Returns 0 on success, -1 for an unknown type, -2 for a NULL argument.
 * // 0x00008656
 */
int device_apply(void *mgr, dev_record_t *rec);

/*
 * device_store_words8808 — write the 8-byte field `src` (two words) into device
 * {0x08,0x88}'s cached record under its semaphore, then run device_apply and
 * release the semaphore. Returns device_apply's status, or -1 if the device is
 * absent/busy. `arg3` mirrors the OEM's unused key-word argument. The 8-byte
 * sibling of device_store_field8c0 (which forwards no result). // 0x00009178
 */
int device_store_words8808(registry_t *reg, const void *src, uint32_t arg3);

/*
 * device_cmd_read87 — open a session, write a 14-byte command frame
 * (0x01 || `payload`[13]) with read-back verify, commit it, then refresh device
 * {dev->id, 0x87} and compare it against that frame. `arg2` mirrors the OEM's
 * unused 2nd register argument. Returns the read result, or -1 on any
 * session/write failure. // 0x00008f76
 */
int device_cmd_read87(dev_handle_t *dev, uint32_t arg2, const void *payload);

/*
 * device_cmd_read91 — the 16-byte twin of device_cmd_read87: writes the 16-byte
 * `payload` to device {dev->id, 0x91}'s 0x40 region (read-back verified),
 * commits, then refreshes via device_read_record91 with `payload` as the
 * expected value. `arg2` mirrors the OEM's unused 2nd register argument. Returns
 * the read result, or -1 on any session/write failure. // 0x000090c0
 */
int device_cmd_read91(dev_handle_t *dev, uint32_t arg2, const void *payload);

/*
 * device_dispatch_command — dispatch one inbound command `msg` against the
 * device manager `mgr`. The 3-byte key selects a device record; its type drives
 * single-shot dispatch, streaming, or multi-fragment reassembly (a 3-slot table
 * at mgr+0x5ac), invoking the record's handler and/or replying through the
 * manager's channel (mgr+0x594). Returns 0 on success, 0xffffffff on a bad
 * key/type/flags or a reply-semaphore timeout. // 0x000041a4
 */
uint32_t device_dispatch_command(int mgr, void *msg);

/*
 * device_apply_task — never-returning FreeRTOS task: block on the device-command
 * queue, and for each dequeued 0x500-byte batch (40 × 0x20-byte slots) write the
 * four signed 16-bit fields of every slot into device {0xc0,0x03,0x00}'s cached
 * record and run device_apply under its semaphore. // 0x00003fe8
 */
void device_apply_task(void *mgr);

/*
 * device_fetch_cache_status9c0 — when cmd[0]==1, fetch a 16-bit status via
 * controlTask_CmdHandler(0x3a), cache it into device {..,0x09,0xc0} and run the
 * apply hook. Returns 0, or the sign-extended handler error. // 0x0000410c
 */
int device_fetch_cache_status9c0(void *reg, uint32_t ctx, const char *cmd, uint32_t arg);

#endif /* USER_ECU_DEVICE_H */
