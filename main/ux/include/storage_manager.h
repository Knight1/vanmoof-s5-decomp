/*
 * storage_manager.h — reconstructed VanMoof S5 i.MX8 `ux` service storage
 * backend (devices/main/ux/src/storage_manager.cpp). Behaviour model of the
 * UXService StorageManager: a key->category map plus per-category value maps,
 * persisted to three on-disk settings files and mirrored over MQTT.
 *
 * Declares the module's own struct + sub-manager helpers that are NOT part of
 * ux_common.h. Everything here is prefixed `storage_`/`sm_`.
 *
 * OEM addresses are quoted in storage_manager.c (program "ux", AArch64,
 * image base 0x100000).
 */
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "ux_common.h"

/* ---- settings categories (the int stored beside every key) ---------------- *
 * cat 0 = global (persisted, published), cat 1 = user (persisted, published),
 * cat 2 = state_context (persisted, NOT published — the `set` gate skips MQTT). */
enum sm_category {
    SM_CAT_GLOBAL = 0,
    SM_CAT_USER   = 1,
    SM_CAT_STATE  = 2
};

/* A stored setting value (modelled nlohmann::json blob held by the value map). */
typedef struct sm_value sm_value;

/*
 * StorageManager object. Field offsets mirror the OEM layout referenced from
 * the decompiled ctor (this+0x40/0x60/0x80 = the three settings-file path
 * roots, this+0x168 = key->category map, this+0x198/0x1c8/0x1f8 = the
 * per-category value maps, this+0x240 = the file-IO helper bound to the bus).
 * The std::map/unordered_map internals are vendor glue — modelled opaquely.
 */
typedef struct storage_manager {
    mqtt_client *bus;          /* this+0x2e: message bus / MQTT publisher       */

    /* three on-disk settings-file path roots, by category index */
    char *path_global;         /* this+0x40  (category 0) */
    char *path_user;           /* this+0x60  (category 1) */
    char *path_state;          /* this+0x80  (category 2) */

    /* persisted device identity fields, loaded from disk at init */
    char *field_ecu_serial;    /* this+0xa0  */
    char *field_bike_id;       /* this+0xc0  */
    char *field_frame_number;  /* this+0xe0  */
    char *field_sku;           /* this+0x100 */
    char *field_cert_pubkey;   /* this+0x120 */

    void *key_category_map;    /* this+0x168: key name -> category int           */
    void *value_map_user;      /* this+0x198: category 1 value store             */
    void *value_map_main;      /* this+0x1c8: combined value store (set/get)     */
    void *value_map_persist;   /* this+0x1f8: per-category serialise store       */

    void *file_io;             /* this+0x240: ofstream/ifstream helper           */
    bool  initialised;         /* this+0x2d: set true at tail of init            */
} storage_manager;

/* ---- value-map / json-value helpers (vendor std::map + nlohmann::json) ----- *
 * Modelled at the call site only; never reconstructed.                        */
bool        sm_keymap_lookup   (storage_manager *sm, const char *key, int *out_cat);
sm_value   *sm_valuemap_at     (storage_manager *sm, int cat, const char *key);
void        sm_valuemap_store  (storage_manager *sm, int cat, const char *key,
                                const json_t *value);
bool        sm_value_is_set    (sm_value *v);
bool        sm_value_assign    (sm_value *v, const json_t *value);
char       *sm_serialise_cat   (storage_manager *sm, int cat);   /* dump map -> text */
void        sm_keymap_insert   (storage_manager *sm, const char *key, int cat,
                                const json_t *def_value);

/* throw helper (FUN_0010dd80 std::runtime_error chain) */
void sm_throw_runtime_error(const char *what);

/* file IO (ofstream/ifstream wrappers bound to this+0x240) */
bool sm_file_write(void *file_io, const char *path, const char *contents);
bool sm_file_read_line(void *file_io, const char *path, char **out_line);

/* JSON-blob walk used by import_user_settings */
bool sm_json_parse(const char *text, json_t *out);
bool sm_json_object_iter(const json_t *obj, void **it,
                         const char **out_key, json_t *out_val);

/* ---- StorageManager public surface (OEM names + ABI) ---------------------- */
void storage_manager_init(storage_manager *sm, mqtt_client *bus,
                          void *ctx, void *settings_ctor_arg,
                          void *cb_a, void *cb_b, void *extra);
int  storage_manager_set(storage_manager *sm, const char *key, const json_t *value);
bool storage_manager_set_if_absent(storage_manager *sm, const char *key,
                                   const json_t *value);
void storage_manager_register_default(storage_manager *sm, const char *key,
                                      int category, const json_t *def_value);
bool storage_manager_persist_category(storage_manager *sm, int category);
bool storage_manager_save_setting(void *file_io, const char *path,
                                  const char *contents);
bool storage_manager_load_string(void *file_io, const char *path, char **out);
void storage_manager_import_user_settings(storage_manager *sm, const char *user_id);

/* identity-field loaders (thin thunks over load_string) */
bool storage_manager_load_field     (storage_manager *sm, char **out);
bool storage_manager_load_field_c0  (storage_manager *sm, char **out);
bool storage_manager_load_field_e0  (storage_manager *sm, char **out);
bool storage_manager_load_field_100 (storage_manager *sm, char **out);
bool storage_manager_load_field_120 (storage_manager *sm, char **out);

#endif /* STORAGE_MANAGER_H */
