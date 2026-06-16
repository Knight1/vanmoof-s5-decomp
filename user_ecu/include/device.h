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
 * A "device" is a fixed 0x2c-byte registry slot keyed by a 3-byte tag
 * {id, type, 0}. The slot's first word points at the device's cached record
 * buffer and its second word is the access semaphore guarding it (see
 * dev_record in device.c). These accessors open a transient I²C bus session
 * (FUN_000028c8), transfer a record to/from the part, and refresh the cached
 * copy under the semaphore.
 *
 * The 16-bit lookup tags seen here are device-class selectors:
 *   0x87  — 14-byte record (sub-address 0x30)   [device_read_record87]
 *   0x91  — 16-byte record (sub-address 0x40)   [device_read_record91]
 *   0x08c0 — 3-byte field write/apply           [device_store_field8c0]
 */

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
 * (FUN_00008656) and release the semaphore. `arg2` mirrors the OEM's unused 3rd
 * register argument. // 0x00008e0a
 */
void device_store_field8c0(registry_t *reg, const void *src, uint32_t arg2);

/*
 * device_cmd_read87 — open a session, write a 14-byte command frame
 * (0x01 || `payload`[13]) with read-back verify, commit it, then refresh device
 * {dev->id, 0x87} and compare it against that frame. `arg2` mirrors the OEM's
 * unused 2nd register argument. Returns the read result, or -1 on any
 * session/write failure. // 0x00008f76
 */
int device_cmd_read87(dev_handle_t *dev, uint32_t arg2, const void *payload);

#endif /* USER_ECU_DEVICE_H */
