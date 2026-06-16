#ifndef USER_ECU_REGISTRY_H
#define USER_ECU_REGISTRY_H

#include <stdint.h>

/*
 * registry.h — fixed-stride entry registry with comparator-based lookup.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS).
 *
 * A small append-only table of fixed 0x2c-byte entries. Each registry owns a
 * comparator used to test a search key against a stored entry; registry_add
 * uses it to reject duplicates before appending. main_SystemInit builds several
 * of these (device / handler tables) during bring-up.
 */

/*
 * Comparator: returns 0 when `key` matches stored `entry`, non-zero otherwise.
 * Invoked as (*cmp)(key, entry) — only those two registers are set up by the
 * caller; the entry pointer addresses a full 0x2c-byte slot.
 */
typedef int (*registry_cmp_fn)(const void *key, const void *entry);

/* In-RAM registry header; entries follow at `slots`, stride REGISTRY_ENTRY_SIZE. */
typedef struct registry {
    uint32_t        count;      /* +0x00 populated entry count            */
    uint32_t        capacity;   /* +0x04 maximum entry count              */
    registry_cmp_fn cmp;        /* +0x08 comparator(key, entry)           */
    uint8_t        *slots;      /* +0x0c base of entry array              */
} registry_t;

/* Each entry is a fixed 0x2c-byte record (movs r2/r7,#0x2c in the OEM). */
#define REGISTRY_ENTRY_SIZE   0x2cu

/* The search key sits 0xc bytes into an entry record. */
#define REGISTRY_KEY_OFFSET   0x0cu

/*
 * Linear search: scan the `count` populated slots, calling reg->cmp(key, slot)
 * on each. Returns the index of the first match, or 0xffffffff if none.
 * // 0x000082c6
 */
uint32_t registry_find(registry_t *reg, const void *key);

/*
 * Append `entry` (a 0x2c-byte record) if the registry has room and no existing
 * slot matches its key (entry + 0xc). On success the entry is copied into the
 * next free slot, `count` is incremented, and 0 is returned. Returns 0xffffffff
 * when reg is NULL, full, or already holds a matching key.
 * // 0x00008594
 */
uint32_t registry_add(registry_t *reg, const void *entry);

/*
 * Look up the entry matching `key` and return a pointer to its 0x2c-byte slot,
 * or NULL if not present. Thin wrapper over registry_find.
 * // 0x000082f2
 */
void *registry_lookup(registry_t *reg, const void *key);

/*
 * Look up an entry by an inline two-word key. Builds {key0, key1} on the stack
 * and forwards to registry_lookup; returns the slot pointer or NULL (also NULL
 * when reg is NULL). This is the common "get device by id" accessor.
 * // 0x0000830a
 */
void *registry_lookup_value(registry_t *reg, uint32_t key0, uint32_t key1);

#endif /* USER_ECU_REGISTRY_H */
