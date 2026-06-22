/*
 * storage_manager.c â reconstructed VanMoof S5 i.MX8 `ux` service settings
 * store (devices/main/ux/src/storage_manager.cpp).
 *
 * The StorageManager owns a key->category table and three category value
 * stores, persisted to three on-disk settings files and mirrored to the MQTT
 * bus. `set` looks the key up (throwing "Key not found" if missing), writes the
 * value, persists the affected category, and â unless the category is the
 * non-published state_context (2) â republishes the new value on a per-key
 * topic. `register_default` seeds the key/category/value tables. The identity
 * fields (ecu_serial, bike_id, frame_number, sku) are loaded from disk on init
 * and published once.
 *
 * Vendor framework (std::map / unordered_map / nlohmann::json / ofstream /
 * ifstream / std::function) is modelled via the sm_* helpers declared in
 * storage_manager.h; only the VanMoof control flow, strings, topics, file
 * paths, log lines, and constants are reconstructed here.
 *
 * Program "ux", AArch64, image base 0x100000.
 */
#include "ux_common.h"
#include "storage_manager.h"

/* sm_on_change — generic MQTT setting-change handler. The OEM binds a
 * per-setting std::function that writes the new value into the category store
 * via storage_manager_set and re-publishes it; that per-topic->key mapping is
 * storage-internal and is modelled here as a single sink (the binding is the
 * reconstructed part, not the vendor std::function body). */
static void sm_on_change(void *ctx, const char *topic, const json_t *payload)
{
    (void)ctx; (void)topic; (void)payload;
}

/* modelled storage helpers (vendor json/string/file-io glue; bodies not reconstructed) */
int  sm_indexof(const char *s, int c);
void sm_valuemap_bind_path(void *sm, int cat, const char *path, const char *user_id);
int  sm_valuemap_category_object(void *sm, int cat, json_t *out);
int  sm_streq(const char *a, const char *b);
void sm_seed_defaults(void *sm);
void sm_str_free(void *p);
void sm_json_str(json_t *j, const char *s);
void sm_json_bool(json_t *j, int v);
void sm_json_null(json_t *j);
void sm_json_int(json_t *j, int v);

#define SM_SRC "devices/main/ux/src/storage_manager.cpp"

/* ===========================================================================
 * storage_manager_register_default  (OEM 0x1195b0)
 *
 * Insert `key` into the key->category map with `category`, and seed both the
 * combined value store (this+0x1c8) and the per-category serialise store
 * (this+0x1f8) with `def_value`. No I/O, no MQTT â pure table population.
 * =========================================================================== */
void storage_manager_register_default(storage_manager *sm, const char *key,
                                      int category, const json_t *def_value)
{
    /* key -> category (insert-or-assign into the unordered_map at +0x168) */
    sm_keymap_insert(sm, key, category, def_value);

    /* seed the combined value store (+0x1c8) used by set()/get() */
    sm_valuemap_store(sm, category, key, def_value);

    /* seed the per-category serialise store (+0x1f8) used by persistCategory() */
    sm_valuemap_store(sm, category, key, def_value);
}

/* ===========================================================================
 * storage_manager_set  (OEM 0x119da0)
 *
 * StorageManager::set(key,value). Look the key up in the key->category map;
 * if absent, throw std::runtime_error("Key not found"). Otherwise store the
 * value into the category's value map, persist that category to disk, and â
 * when the category requires publishing (category != 2, the state_context
 * file) â republish on the per-key MQTT topic (qos 5, retain 1).
 *
 * Returns the persistCategory() result (non-zero on successful write).
 * =========================================================================== */
int storage_manager_set(storage_manager *sm, const char *key, const json_t *value)
{
    int cat;

    /* lookup in key->category map (this+0x168); throw if not present */
    if (!sm_keymap_lookup(sm, key, &cat)) {
        /* runtime_error("Key not found") â FUN_0010dd80 throw chain */
        sm_throw_runtime_error("Key not found");
        return 0; /* not reached */
    }

    /* store value into the category's value map (this+0x1c8) */
    sm_valuemap_store(sm, cat, key, value);

    /* serialise + write the affected category file */
    int wrote = storage_manager_persist_category(sm, cat) ? 1 : 0;

    /* category 2 (state_context) is persisted but never published */
    if (wrote != 0 && cat != SM_CAT_STATE) {
        /* publish on the per-key topic "<key>" (qos 5, retain 1).
         * OEM concatenates the manager's topic prefix with the key. */
        mqtt_publish_json(sm->bus, key, value, 5, 1);
    }

    return wrote;
}

/* ===========================================================================
 * storage_manager_set_if_absent  (OEM 0x11a0d0)
 *
 * Insert-only set. If the key is unknown, log "Key not found: %s" (line 0x81)
 * and return false without storing. If the key exists and already holds a
 * value, return false (no overwrite). Otherwise delegate to set() to store,
 * persist and publish.
 * =========================================================================== */
bool storage_manager_set_if_absent(storage_manager *sm, const char *key,
                                   const json_t *value)
{
    int cat;

    if (!sm_keymap_lookup(sm, key, &cat)) {
        common_logf(SM_SRC, 0x81, LOG_DEBUG, "Key not found: %s\n", key);
        return false;
    }

    /* if a value is already present for this key, do not overwrite */
    sm_value *existing = sm_valuemap_at(sm, cat, key);
    if (existing != NULL && sm_value_is_set(existing)) {
        if (sm_value_assign(existing, value)) {
            /* value differed â commit via the full set() path */
            return storage_manager_set(sm, key, value) != 0;
        }
        return false;
    }

    /* absent â store via the full set() path (persist + publish) */
    return storage_manager_set(sm, key, value) != 0;
}

/* ===========================================================================
 * storage_manager_persist_category  (OEM 0x1192b0)
 *
 * Serialise the value map for one category and write it to that category's
 * on-disk settings file:
 *   cat 0 -> path_global (this+0x40)
 *   cat 1 -> path_user   (this+0x60)
 *   cat 2 -> path_state  (this+0x80)
 * For category 0 the OEM looks the category-0 sub-map up twice (dumps to text,
 * then re-reads the bucket for the write) â modelled here as a single dump.
 * Returns the save_setting() result.
 * =========================================================================== */
bool storage_manager_persist_category(storage_manager *sm, int category)
{
    const char *path;

    switch (category) {
    case SM_CAT_GLOBAL: path = sm->path_global; break;
    case SM_CAT_USER:   path = sm->path_user;   break;
    default:            path = sm->path_state;  break; /* category 2 */
    }

    /* dump the category's value map to its serialised text form */
    char *text = sm_serialise_cat(sm, category);

    bool ok = storage_manager_save_setting(sm->file_io, path, text);

    json_dump_free(text);
    return ok;
}

/* ===========================================================================
 * storage_manager_save_setting  (OEM 0x118b50)
 *
 * Open `path` as an ofstream and write `contents`. On open/stream failure log
 * "Could not save settings" (line 0x2e) and return false. The OEM builds the
 * filename, opens std::ofstream, streams the payload via operator<<, checks the
 * stream state, then closes. Returns true on success.
 * =========================================================================== */
bool storage_manager_save_setting(void *file_io, const char *path,
                                  const char *contents)
{
    if (!sm_file_write(file_io, path, contents)) {
        common_logf(SM_SRC, 0x2e, LOG_DEBUG, "Could not save settings");
        return false;
    }
    return true;
}

/* ===========================================================================
 * storage_manager_load_string  (OEM 0x117e10)
 *
 * Open `path` as an ifstream and getline() one record into *out. If the file
 * is missing / the stream's failbit|badbit is set on open, log "File does not
 * exist" (line 0x46) and return false. On a valid open but failed read, log
 * "Could not load string" (line 0x4b) and return false. Returns true with *out
 * populated on success.
 * =========================================================================== */
bool storage_manager_load_string(void *file_io, const char *path, char **out)
{
    /* probe open: (failbit|badbit) -> file does not exist */
    if (!sm_file_read_line(file_io, path, out)) {
        common_logf(SM_SRC, 0x46, LOG_DEBUG, "File does not exist");
        return false;
    }

    if (*out == NULL) {
        common_logf(SM_SRC, 0x4b, LOG_DEBUG, "Could not load string");
        return false;
    }

    return true;
}

/* ===========================================================================
 * Identity-field loaders (OEM 0x1182f0 / 0x118300 / 0x118310 / 0x118320 /
 * 0x118330). Thin thunks that load each persisted device field from disk via
 * load_string, into the StorageManager field slot:
 *   load_field      -> field_ecu_serial   (this+0xa0)
 *   load_field_c0   -> field_bike_id      (this+0xc0)
 *   load_field_e0   -> field_frame_number (this+0xe0)
 *   load_field_100  -> field_sku          (this+0x100)
 *   load_field_120  -> field_cert_pubkey  (this+0x120)
 * Each passes the file-IO helper at this+0x240.
 * =========================================================================== */
bool storage_manager_load_field(storage_manager *sm, char **out)      /* 0x1182f0 */
{
    return storage_manager_load_string(sm->file_io, sm->field_ecu_serial, out);
}

bool storage_manager_load_field_c0(storage_manager *sm, char **out)   /* 0x118300 */
{
    return storage_manager_load_string(sm->file_io, sm->field_bike_id, out);
}

bool storage_manager_load_field_e0(storage_manager *sm, char **out)   /* 0x118310 */
{
    return storage_manager_load_string(sm->file_io, sm->field_frame_number, out);
}

bool storage_manager_load_field_100(storage_manager *sm, char **out)  /* 0x118320 */
{
    return storage_manager_load_string(sm->file_io, sm->field_sku, out);
}

bool storage_manager_load_field_120(storage_manager *sm, char **out)  /* 0x118330 */
{
    return storage_manager_load_string(sm->file_io, sm->field_cert_pubkey, out);
}

/* ===========================================================================
 * storage_manager_import_user_settings  (OEM 0x11ad40)
 *
 * Validate the user_id and seed the user-bound settings paths/keys.
 *
 * 1. The user_id must contain no '.' (0x2e) or '/' (0x2f) â either makes the id
 *    an invalid path token; log "Invalid character in user_id" (line 0x91) and
 *    abort.
 * 2. Build the per-user settings file paths from path_global/path_user/
 *    path_state and the user_id, registering them into the value stores.
 * 3. Parse the persisted user-settings JSON blobs for categories 0 and 1, and
 *    for every key whose value compares non-equal to the seed "0"
 *    (DAT_001b12b0) insert it (set_if_absent) into the live store; on a
 *    set_if_absent miss, fall back to storing the raw scalar under that key.
 * =========================================================================== */
void storage_manager_import_user_settings(storage_manager *sm, const char *user_id)
{
    /* reject ids containing a path separator */
    if (sm_indexof(user_id, '.') != -1 || sm_indexof(user_id, '/') != -1) {
        common_logf(SM_SRC, 0x91, LOG_DEBUG, "Invalid character in user_id");
        return;
    }

    /* --- bind the per-user file paths into the value stores (cat 0/1/2) --- */
    /* category 0: "<path_global>" keyed by user_id */
    sm_valuemap_bind_path(sm, SM_CAT_GLOBAL, sm->path_global, user_id);
    /* category 1: "<path_user>"  keyed by user_id */
    sm_valuemap_bind_path(sm, SM_CAT_USER,   sm->path_user,   user_id);
    /* category 2: "<path_state>" keyed by user_id */
    sm_valuemap_bind_path(sm, SM_CAT_STATE,  sm->path_state,  user_id);

    /* --- walk the parsed per-category settings objects --- */
    static const char *const kSeed = "0";   /* DAT_001b12b0: skip values == "0" */

    for (int cat = SM_CAT_GLOBAL; cat <= SM_CAT_STATE; ++cat) {
        json_t obj;
        if (!sm_valuemap_category_object(sm, cat, &obj))
            continue;

        void *it = NULL;
        const char *key;
        json_t val;
        while (sm_json_object_iter(&obj, &it, &key, &val)) {
            const char *s = json_get_string(&val);
            if (s != NULL && sm_streq(s, kSeed)) {
                json_free(&val);
                continue;   /* unchanged from seed "0" â skip */
            }

            if (!storage_manager_set_if_absent(sm, key, &val)) {
                /* miss: fall back to storing the raw scalar under the key */
                sm_valuemap_store(sm, cat, key, &val);
                storage_manager_set_if_absent(sm, key, &val);
            }
            json_free(&val);
        }
        json_free(&obj);
    }
}

/* ===========================================================================
 * storage_manager_init  (OEM 0x141ea0)
 *
 * StorageManager ctor. Seeds the in-memory default key table, loads + publishes
 * the persisted device-identity fields, registers each setting key with its
 * category, imports the user settings, and subscribes to the setting-change
 * topics on the bus.
 *
 * Note: the OEM ctor also embeds a number of sub-managers (light/ride/alarm/
 * findmy/ble/update) inline; those are constructed by their own modules and are
 * not part of the storage reconstruction â modelled here only as the storage
 * portion of the object.
 * =========================================================================== */
void storage_manager_init(storage_manager *sm, mqtt_client *bus,
                          void *ctx, void *settings_ctor_arg,
                          void *cb_a, void *cb_b, void *extra)
{
    (void)ctx; (void)settings_ctor_arg; (void)cb_a; (void)cb_b; (void)extra;

    /* zero-initialise the storage portion of the object */
    sm->bus              = bus;
    sm->path_global      = NULL;
    sm->path_user        = NULL;
    sm->path_state       = NULL;
    sm->field_ecu_serial = NULL;
    sm->field_bike_id    = NULL;
    sm->field_frame_number = NULL;
    sm->field_sku        = NULL;
    sm->field_cert_pubkey = NULL;
    sm->key_category_map  = NULL;
    sm->value_map_user    = NULL;
    sm->value_map_main    = NULL;
    sm->value_map_persist = NULL;
    sm->file_io           = NULL;
    sm->initialised       = false;

    /* --- seed the default key strings + the "shipping" timeout (6000) ----- *
     * The ctor stores the literal key names and a couple of inline defaults
     * (e.g. animation_theme default "ride_animation_right", shipping=6000)    */
    sm_seed_defaults(sm);                 /* installs path roots + file_io */

    /* --- load + publish persisted identity fields --------------------------- *
     * Each field is loaded from disk; on success it is published once on its
     * "info/<key>" topic (qos 1, retain 1).                                        */
    char *fld = NULL;

    if (storage_manager_load_field(sm, &fld))           /* this+0xa0  */
        mqtt_publish_str(sm->bus, "info/ecu_serial", fld, 1, 1);
    sm_str_free(&fld);

    if (storage_manager_load_field_e0(sm, &fld))        /* this+0xe0  */
        mqtt_publish_str(sm->bus, "info/frame_number", fld, 1, 1);
    sm_str_free(&fld);

    if (storage_manager_load_field_c0(sm, &fld))        /* this+0xc0  */
        mqtt_publish_str(sm->bus, "info/bike_id", fld, 1, 1);
    sm_str_free(&fld);

    if (storage_manager_load_field_100(sm, &fld))       /* this+0x100 */
        mqtt_publish_str(sm->bus, "info/sku", fld, 1, 1);
    sm_str_free(&fld);

    /* The certificate public key is NOT loaded from disk: the OEM publishes a
     * compiled-in default 64-char hex constant (DAT_001b1258) unconditionally,
     * qos 1 retain 1 (init never calls load_field_120 / 0x118330). */
    mqtt_publish_str(sm->bus, "ble/certificate_public_key",
                     "29b1f31c07d1c63b124057ebe75a0bc0796259722e5dd9a9a9302ae2061184a0",
                     1, 1);

    /* --- register every setting key with its category ----------------------- *
     * (default values modelled; categories verbatim from the ctor)            */
    json_t dv; /* reused default-value blob */

    sm_json_str(&dv, "ride_animation_right");
    storage_manager_register_default(sm, "animation_theme", SM_CAT_USER, &dv);
    json_free(&dv);

    sm_json_bool(&dv, true);
    storage_manager_register_default(sm, "ride_animation_right", SM_CAT_USER, &dv);
    json_free(&dv);

    sm_json_null(&dv);
    storage_manager_register_default(sm, "brake_lights", SM_CAT_GLOBAL, &dv);
    json_free(&dv);

    sm_json_int(&dv, 100);
    storage_manager_register_default(sm, "turning_lights", SM_CAT_USER, &dv);
    json_free(&dv);

    sm_json_null(&dv);
    storage_manager_register_default(sm, "backup_unlock_code", SM_CAT_GLOBAL, &dv);
    json_free(&dv);

    sm_json_null(&dv);
    storage_manager_register_default(sm, "bell_sound", SM_CAT_GLOBAL, &dv);
    json_free(&dv);

    sm_json_null(&dv);
    storage_manager_register_default(sm, "master_volume", SM_CAT_GLOBAL, &dv);
    json_free(&dv);

    sm_json_null(&dv);
    storage_manager_register_default(sm, "assist_level", SM_CAT_GLOBAL, &dv);
    json_free(&dv);

    sm_json_bool(&dv, true);
    storage_manager_register_default(sm, "region", SM_CAT_STATE, &dv);
    json_free(&dv);

    /* --- import the user-bound settings (user_id = "default") -------------- */
    storage_manager_import_user_settings(sm, "default");

    /* --- subscribe to the setting-change topics on the bus ----------------- *
     * (handlers are storage-publish thunks; modelled at the call site)        */
    mqtt_subscribe(sm->bus, "ux/sound/play",          sm_on_change, sm);
    mqtt_subscribe(sm->bus, "ux/lock/unlock",         sm_on_change, sm);
    mqtt_subscribe(sm->bus, "ux/lock/software_lock",  sm_on_change, sm);
    mqtt_subscribe(sm->bus, "ux/alarm/state",         sm_on_change, sm);
    mqtt_subscribe(sm->bus,
        "update/background_update/finished/rearlight", sm_on_change, sm);

    sm->initialised = true;   /* this+0x2d = 1 */
}
