#include "update_common.h"

/* ===== module-local framework model (opaque object + macros + externs) ===== */
/* ================================================================== *
 *  The UpdateService object (0x298 bytes), accessed by OEM offset.
 *  Modelled as an OPAQUE byte block + accessor macros (like PowerService);
 *  sub-objects (mutexes, std::string, tables) are reached as pointers via
 *  US_SUBOBJ and never embedded by value.
 * ================================================================== */
typedef struct update_service { uint8_t _raw[0x298]; } update_service;

#define US_FIELD(s,off,ty)  (*(ty *)((uint8_t *)(s)->_raw + (off)))
#define US_SUBOBJ(s,off)    ((void *)((uint8_t *)(s)->_raw + (off)))

#define US_start_delay(s)        US_FIELD(s,0xa8,int64_t)   /* settle before stage load   */
#define US_poll_delay(s)         US_FIELD(s,0xb0,int64_t)   /* per-retry / commit settle   */
#define US_reboot_delay(s)       US_FIELD(s,0xb8,int64_t)   /* settle before exec-reboot   */
#define US_commit_delay(s)       US_FIELD(s,0xc0,int64_t)   /* (reserved)                  */
#define US_stage2_delay(s)       US_FIELD(s,0xc8,int64_t)   /* settle before stage-2 work  */
#define US_state_mutex(s)        US_SUBOBJ(s,0xd0)          /* run_state_machine guard     */
#define US_state_thread(s)       US_FIELD(s,0x100,void *)   /* std::thread handle          */
#define US_mqtt(s)               US_FIELD(s,0x108,void *)   /* IMQTTClient publish bus     */
#define US_manifest(s)           US_FIELD(s,0x110,void *)   /* manifest / device vector    */
#define US_factory(s)            US_FIELD(s,0x118,void *)   /* UpdateClientFactory         */
#define US_boot(s)               US_FIELD(s,0x120,void *)   /* A/B boot-control (vtable)   */
#define US_total_steps(s)        US_FIELD(s,0x130,int32_t)  /* TOTAL STEPS reported        */
#define US_prev_state(s)         US_FIELD(s,0x134,int32_t)  /* power state to restore      */
#define US_vclient(s)            US_FIELD(s,0x138,void *)   /* version client (getVersion) */
#define US_stage(s)              US_FIELD(s,0x140,int8_t)   /* current update_stage        */
#define US_metadata(s)           ((str_t *)US_SUBOBJ(s,0x148)) /* extracted metadata string*/
#define US_metadata_len(s)       US_FIELD(s,0x150,size_t)   /* metadata std::string size   */
#define US_state_client(s)       US_SUBOBJ(s,0x158)         /* per-thread state client     */
#define US_finished_flag_path(s) (*US_FIELD(s,0x170,char **)) /* char* finished-flag path  */
#define US_bg_mutex(s)           US_SUBOBJ(s,0x228)         /* background-table guard      */
#define US_bg_table(s)           US_SUBOBJ(s,0x258)         /* background update table     */
#define US_pending_count(s)      US_FIELD(s,0x280,size_t)   /* outstanding device count    */
#define US_battery_soc(s)        US_FIELD(s,0x290,uint16_t) /* primary SoC %% (gate < 11)  */

/* logger / nanosleep-with-EINTR-restart (US_poll_delay etc are ms). */
void nanosleep_ms(int64_t ms);                       /* models the OEM open-coded nanosleep loop */
/* ---- std::string model (one pointer per field; never embedded by value) ---- */
typedef struct str { uint8_t _raw[0x20]; } str_t;     /* OEM std::__cxx11::string */
void        str_init_empty(str_t *s);                 /* construct ""  (&DAT_00171c20) */
void        str_init_cstr(str_t *s, const char *c);   /* construct from C literal */
void        str_destroy(str_t *s);                    /* ~string (frees heap rep) */
const char *str_cstr(const str_t *s);                 /* .c_str() */
size_t      str_len(const str_t *s);                  /* .size() */
bool        str_empty(const str_t *s);                /* .empty() */
size_t      str_find(const str_t *s, const char *needle); /* .find(); (size_t)-1 == npos */
/* ---- nlohmann::json model (opaque; build + publish, never transcribed) ---- */
json_t *json_object_new(void);                        /* {} */
json_t *json_string_new(const char *v);               /* "..." json string node */
void    json_set_int (json_t *o, const char *key, int v);
void    json_set_bool(json_t *o, const char *key, bool v);
void    json_set_str (json_t *o, const char *key, const char *v);
void    json_release(json_t *o);
/* ---- MQTT bus (US_mqtt vtable: +0x10 sub, +0x18 unsub, +0x20 publish) ---- */
typedef void (*mqtt_handler)(void *ctx, const char *topic, const json_t *msg);
void mqtt_publish_json(void *mqtt, const char *topic, const json_t *payload,
                       int qos, int retain);          /* vtable +0x20 */
void mqtt_subscribe (void *mqtt, const char *topic, mqtt_handler h, void *ctx); /* +0x10 */
void mqtt_unsubscribe(void *mqtt, const char *topic); /* +0x18 */
/* ---- boot-control object (US_boot, +0x120). Each call is a vtable slot. ---- */
typedef struct boot_control boot_control;
int  boot_get_stage(void *boot);                      /* +0x10 -> update_stage */
bool boot_verify_version(void *boot, const void *req);/* +0x20 -> nonzero on fail */
bool boot_move_to_fota(void *boot, const str_t *path);/* +0x28 */
bool boot_split(void *boot);                          /* +0x38 */
bool boot_mount(void *boot);                          /* +0x40 / +0x90 (restore mount) */
bool boot_extract_scripting(void *boot);              /* +0x48 */
void boot_read_metadata(void *boot, str_t *out);      /* +0x50 (writes std::string) */
bool boot_umount(void *boot);                         /* +0x58 */
bool boot_pre_install(void *boot);                    /* +0x60 */
bool boot_install(void *boot);                        /* +0x68 */
bool boot_commit(void *boot);                         /* +0x70 */
bool boot_rollback(void *boot);                       /* +0x78 */
bool boot_post_install(void *boot);                   /* +0x80 */
bool boot_exec_reboot(void *boot);                    /* +0x88 */
/* ---- version client (US_vclient, +0x138). getVersion {ok,value}; supplier byte. ---- */
typedef struct version_client version_client;
bool vclient_get_version(void *vc, const str_t *device, int *out); /* vtable +0x10 */
char vclient_get_supplier(void *vc, const char *device);          /* vtable +0x20 -> 'P'/'f'/'D'/... */
const char *version_str(int packed);                  /* FUN_0015abc0: packed semver -> text */
/* ---- update-client factory (US_factory, +0x118) + concrete client vtable ---- */
typedef struct update_client update_client;
typedef struct manifest_entry manifest_entry;  /* fwd (defined again below; C11 allows) */
update_client *factory_get_client(void *factory, const manifest_entry *entry); /* +0x10 */
int  update_client_update(update_client *c);          /* vtable +0x10 -> 0 ok */
void update_client_release(update_client *c);         /* vtable +8 (delete) */
/* ---- per-thread state client (US_state_client, +0x158). ---- */
void state_client_request_state(update_service *s, int state); /* OEM 0x156870 */
/* ---- manifest device vector (US_manifest, +0x110: begin +8 / end +0x10). ---- */
typedef struct manifest_entry manifest_entry;
size_t                manifest_device_count(void *manifest);
const manifest_entry *manifest_device_begin(void *manifest);    /* +8  */
const manifest_entry *manifest_device_end  (void *manifest);    /* +0x10 */
const manifest_entry *manifest_device_next (const manifest_entry *e); /* ++ (stride 0x60) */
void manifest_entry_name(const manifest_entry *e, str_t *out);  /* FUN_00123ae0: device name */
bool manifest_entry_version(const manifest_entry *e, int *out); /* wanted version, false if absent */
bool manifest_entry_allowed_to_skip(const manifest_entry *e);  /* +0x58 */
bool manifest_entry_skip_rollback (const manifest_entry *e);   /* +0x59 */
/* ---- background update table (US_bg_table +0x258, guarded by US_bg_mutex +0x228) ---- */
void bg_mutex_lock  (void *m);
void bg_mutex_unlock(void *m);
void bg_table_add(void *table, const str_t *device);  /* FUN_0012d100 */
/* family classifiers used by run_device_updates (FUN_0012cc30 set membership). */
bool name_in_charger_family(const str_t *name);       /* charger / _liteon_* set */
bool name_in_modem_family  (const str_t *name);       /* modem / charger background set */
/* publish the {"start":true,"trigger":true} message to update/background_update/<name>. */
void publish_background_update_trigger(update_service *s, const str_t *name);
/* device/+/status handler bound by wait_for_devices (OEM FUN_00125960). */
void on_device_status(void *ctx, const char *topic, const json_t *msg);
/* misc framework helpers. */
int64_t  monotonic_ns(void);                          /* std::chrono::system_clock::now() ns */
int      file_count_lines(const char *path);          /* '\n' count for count_total_steps */
void     state_mutex_lock  (void *m);                 /* pthread_mutex_lock (US_state_mutex) */
void     state_mutex_unlock(void *m);
/* ========================================================================== */

/*
 * devices/main/update/src/update_service.cpp  (reconstructed)
 *
 * VanMoof S5/A5 OTA 'update' service top-level state machine. ELF: /usr/bin/update
 * (AArch64, C++). Image base 0x100000. OEM addresses are quoted per-function.
 *
 * The UpdateService object is offset-based in the binary; it is modelled here as
 * an opaque byte block (update_service._raw[0x298]) accessed through the US_*
 * accessor macros (see structs[]/externs[]), exactly like the power service.
 * The boot-control object (+0x120), the version client (+0x138), the MQTT bus
 * (+0x108), the per-thread state client (+0x158) and the manifest device vector
 * (+0x110) are reached through small vtables; the std::string / nlohmann::json /
 * std::thread / mosquitto plumbing is modelled through the externs, never
 * transcribed. Only the VanMoof update sequencing is translated.
 *
 * Boot-control vtable slots (US_boot(s)):
 *   +0x10 get_stage()          +0x28 move_to_fota(path)   +0x38 split()
 *   +0x40 mount()              +0x48 extract_scripting()  +0x50 read_metadata()
 *   +0x58 umount()             +0x60 pre_install()        +0x68 install()
 *   +0x70 commit()             +0x78 rollback()           +0x80 post_install()
 *   +0x88 exec_reboot()        +0x90 mount() [restore]    +0x20 verify_version()
 */

#define SRC "devices/main/update/src/update_service.cpp"

/* ------------------------------------------------------------------ *
 *  OEM .bss std::string token globals (built in _INIT_8 @0x10d9a0).
 *  Modelled as C strings; used as substring needles by the device loop.
 * ------------------------------------------------------------------ */
static const char *const TOK_BATTERY_PRIMARY           = "battery_primary"; /* DAT_001a22b8 */
static const char *const TOK_BATTERY_PRIMARY_PANASONIC = "battery_primary_panasonic"; /* DAT_001a2378 */
static const char *const TOK_BATTERY_PRIMARY_DYNAPACK  = "battery_primary_dynapack";  /* DAT_001a2398 */
static const char *const TOK_CHARGER                   = "charger";         /* DAT_001a2358 */

/* progress JSON object keys (DAT literals @0x1728c0 / 0x1728d0). */
/* "stage","step","total","status","error","device"; "skip" for status==SKIPPED. */

/* a per-step short delay (US_poll_delay) is given in ms; nanosleep_ms wraps the
 * EINTR-restart nanosleep the OEM open-codes at every wait site. */

/* ================================================================== *
 *  publish_progress                                OEM 0x00127eb0
 *
 *  Builds the nlohmann::json progress object and publishes it retained to
 *  "update/progress":
 *      { "stage": <US_stage>, "step": <step>, "total": <US_total_steps>,
 *        "status": <status> }
 *  When status == -3 (UPD_SKIPPED encoded as the OEM sentinel 0xfd) it adds
 *      "skip": true
 *  When status != 0 (any error) it adds
 *      "error": <error_code>
 *  When the device name string is non-empty it adds
 *      "device": <device_name>
 *  step/total/error are signed-char/byte in the OEM ABI.
 * ================================================================== */
static void publish_progress(update_service *s, unsigned step,
                             signed char status, unsigned char error,
                             const str_t *device_name)
{
    json_t *obj = json_object_new();

    json_set_int (obj, "stage",  (int)US_stage(s));
    json_set_int (obj, "step",   (int)step);
    json_set_int (obj, "total",  (int)US_total_steps(s));
    json_set_int (obj, "status", (int)status);

    if (status == -3) {                       /* SKIPPED sentinel */
        json_set_bool(obj, "skip", true);
    } else if (status != 0) {                 /* any failure carries an error */
        json_set_int(obj, "error", (int)error);
    }

    if (!str_empty(device_name)) {
        json_set_str(obj, "device", str_cstr(device_name));
    }

    /* mqtt publish (US_mqtt vtable+0x20): topic, payload, qos 5, retain 1. */
    mqtt_publish_json(US_mqtt(s), "update/progress", obj, 5, 1);
    json_release(obj);
}

/* ================================================================== *
 *  count_total_steps                               OEM 0x001246f0
 *
 *  Computes the "TOTAL STEPS" reported up-front, keyed by the boot-control
 *  stage already loaded into US_stage(s):
 *    stage 2 or 4 (A/B install / rollback-into-stage-2):
 *        9 + (number of manifest devices)
 *    stage 1:
 *        9
 *    otherwise (fresh FOTA install, US_metadata present):
 *        8 + (number of '\n'-terminated lines in the metadata file at
 *             US_metadata path)  -- i.e. 8 + per-device manifest line count.
 *    otherwise (no metadata):
 *        9
 *  The OEM opens US_metadata (an ifstream) and counts newlines; modelled by
 *  file_count_lines(path).
 * ================================================================== */
static int count_total_steps(update_service *s, signed char stage)
{
    /* (stage-2) & 0xfd == 0  <=>  stage in {2,4} */
    if ((((int)stage - 2) & 0xfd) == 0) {
        return 9 + (int)manifest_device_count(US_manifest(s));
    }
    if (stage == 1) {
        return 9;
    }
    if (US_metadata_len(s) != 0) {
        return 8 + file_count_lines(str_cstr(US_metadata(s)));
    }
    return 9;
}

/* ================================================================== *
 *  write_finished_flag                             OEM 0x00124ac0
 *
 *  Truncate-writes the integer `value` to the finished-flag file whose path
 *  is at US_finished_flag_path(s) (a char* held at +0x170). Logs on failure.
 *  (ofstream(path, trunc) << value).
 * ================================================================== */
static void write_finished_flag(update_service *s, int value)
{
    FILE *f = fopen(US_finished_flag_path(s), "w");
    if (f == NULL) {
        common_logf(SRC, 0x1f5, LOG_ERR, "Unable to create finished_flag file");
        return;
    }
    fprintf(f, "%d", value);
    fclose(f);
}

/* ================================================================== *
 *  prepare_and_verify_package                      OEM 0x00124d70
 *
 *  Move the parked upgrade blob into the FOTA location, verify it, mount the
 *  target rootfs, pull out the metadata, and umount again. Stores the metadata
 *  string into US_metadata(s). Returns 0 on success, -1 on any failure.
 *
 *  boot-control vtable: +0x28 move_to_fota(path), +0x20 verify_version(req),
 *                       +0x40 mount, +0x50 read_metadata(&out), +0x58 umount.
 * ================================================================== */
int update_service_prepare_and_verify_package(update_service *s,
                                              const str_t *dest_path,
                                              const void *version_req)
{
    void *boot = US_boot(s);

    /* Only move when a destination path string was supplied (len > 1). */
    if (str_len(dest_path) > 1) {
        if (boot_move_to_fota(boot, dest_path)) {           /* vtable +0x28 */
            common_logf(SRC, 0x1fe, LOG_ERR,
                        "Failed moving file to fota location");
            return -1;
        }
    }

    common_logf(SRC, 0x203, LOG_INFO, "Verifying integrity of upgrade file");
    if (boot_verify_version(boot, version_req)) {           /* vtable +0x20 */
        common_logf(SRC, 0x206, LOG_ERR, "Unable to read package version");
        return -1;
    }

    if (boot_mount(boot)) {                                 /* vtable +0x40 */
        common_logf(SRC, 0x20c, LOG_ERR, "Unable to mount root");
        return -1;
    }

    /* read the metadata into US_metadata (std::string move-assign). */
    boot_read_metadata(boot, US_metadata(s));               /* vtable +0x50 */
    if (US_metadata_len(s) == 0) {
        common_logf(SRC, 0x212, LOG_ERR, "Unable to extract metadata");
        return -1;
    }

    if (boot_umount(boot)) {                                /* vtable +0x58 */
        common_logf(SRC, 0x218, LOG_ERR, "Unable to umount root");
        return -1;
    }
    return 0;
}

/* ================================================================== *
 *  run_fota_install_sequence                        OEM 0x00129600
 *
 *  The 8-step i.MX8 FOTA install pipeline. Each step calls a boot-control
 *  vtable slot; on success it publishes progress(step, 0, 0); on failure it
 *  logs and publishes progress(step, -1, <error-code>) and returns -1.
 *
 *    step 1  split()          vtable+0x38   err 3   "Failed splitting the update file"
 *    step 2  mount()          vtable+0x40   err 4   "Failed mounting the rootfs"
 *    step 3  extract()        vtable+0x48   err 6   "Failed extracting the upgrade scripting"
 *    step 4  umount()         vtable+0x58   err 8   "Failed umounting the rootfs"
 *    step 5  pre_install()    vtable+0x60   err 9   "Failed executing the pre-install"
 *    step 6  install()        vtable+0x68   err 10  "Failed installing the FOTA"
 *    step 7  post_install()   vtable+0x80   err 11  "Failed executing the post-install"
 *    step 8  exec_reboot()    vtable+0x88   err 12  "Failed to reboot"
 *
 *  All progress device names are empty here (DAT_00171c20 -> "").
 * ================================================================== */
int update_service_run_fota_install_sequence(update_service *s)
{
    void *boot = US_boot(s);
    str_t dev; str_init_empty(&dev);   /* &DAT_00171c20 == "" */

    /* step 1: split() -- note: vtable+0x38 is invoked with a 0 argument. */
    if (boot_split(boot)) {                                 /* vtable +0x38 */
        common_logf(SRC, 0x2d8, LOG_ERR, "Failed splitting the update file");
        publish_progress(s, 1, -1, 3, &dev);
        goto fail;
    }
    publish_progress(s, 1, 0, 0, &dev);

    if (boot_mount(boot)) {                                 /* vtable +0x40 */
        common_logf(SRC, 0x2e2, LOG_ERR, "Failed mounting the rootfs");
        publish_progress(s, 2, -1, 4, &dev);
        goto fail;
    }
    publish_progress(s, 2, 0, 0, &dev);

    if (boot_extract_scripting(boot)) {                     /* vtable +0x48 */
        common_logf(SRC, 0x2ec, LOG_ERR,
                    "Failed extracting the upgrade scripting");
        publish_progress(s, 3, -1, 6, &dev);
        goto fail;
    }
    publish_progress(s, 3, 0, 0, &dev);

    if (boot_umount(boot)) {                                /* vtable +0x58 */
        common_logf(SRC, 0x2f6, LOG_ERR, "Failed umounting the rootfs");
        publish_progress(s, 4, -1, 8, &dev);
        goto fail;
    }
    publish_progress(s, 4, 0, 0, &dev);

    if (boot_pre_install(boot)) {                           /* vtable +0x60 */
        common_logf(SRC, 0x300, LOG_ERR, "Failed executing the pre-install");
        publish_progress(s, 5, -1, 9, &dev);
        goto fail;
    }
    publish_progress(s, 5, 0, 0, &dev);

    if (boot_install(boot)) {                               /* vtable +0x68 */
        common_logf(SRC, 0x30a, LOG_ERR, "Failed installing the FOTA");
        publish_progress(s, 6, -1, 10, &dev);
        goto fail;
    }
    publish_progress(s, 6, 0, 0, &dev);

    if (boot_post_install(boot)) {                          /* vtable +0x80 */
        common_logf(SRC, 0x314, LOG_ERR, "Failed executing the post-install");
        publish_progress(s, 7, -1, 11, &dev);
        goto fail;
    }
    publish_progress(s, 7, 0, 0, &dev);

    if (boot_exec_reboot(boot)) {                           /* vtable +0x88 */
        common_logf(SRC, 0x31e, LOG_ERR, "Failed to reboot");
        publish_progress(s, 8, -1, 12, &dev);
        goto fail;
    }
    publish_progress(s, 8, 0, 0, &dev);

    str_destroy(&dev);
    return 0;

fail:
    str_destroy(&dev);
    return -1;
}

/* ================================================================== *
 *  handle_device_update                             OEM 0x00125040
 *
 *  Per manifest device: compare the wanted version (from the manifest entry)
 *  against the running version reported by the version client, then -- if they
 *  differ -- build an update client from the factory and flash up to 3 times.
 *
 *  - the version-client lookup key is the device name, except battery_primary*
 *    devices collapse to the literal "battery_primary".
 *  - version-client getVersion (US_vclient vtable+0x10) returns {ok, value}.
 *  - if the running version is unreadable, retry US_poll_delay 3x; if still
 *    unreadable, force the update ("trying to force update").
 *  - the manifest entry must carry a wanted version (cStack_9c); otherwise the
 *    OEM throws (modelled as the error path).
 *  - equal versions  -> "No update required" -> return 0.
 *  - else            -> factory.GetUpdateClient (US_factory vtable+0x10);
 *                       client->Update() (vtable+0x10) up to 3 trials.
 *
 *  Returns 0 on success / no-update, -1 on failure.
 * ================================================================== */
int update_service_handle_device_update(update_service *s,
                                        const manifest_entry *entry)
{
    str_t name;          /* version-client lookup key */
    str_t dev;           /* device display name (for logs) */
    bool  have_running;
    bool  have_wanted;
    int   running_ver = 0;
    int   wanted_ver  = 0;
    int   trial;

    /* lookup key: battery_primary* -> "battery_primary", else the device name. */
    manifest_entry_name(entry, &dev);                       /* FUN_00123ae0 */
    if (str_find(&dev, TOK_BATTERY_PRIMARY) != (size_t)-1) {
        str_init_cstr(&name, TOK_BATTERY_PRIMARY);          /* DAT_001a22b8 */
    } else {
        manifest_entry_name(entry, &name);
    }
    str_destroy(&dev);

    /* running version from the version client (vtable+0x10 -> {ok, value}). */
    have_running = vclient_get_version(US_vclient(s), &name, &running_ver);
    have_wanted  = manifest_entry_version(entry, &wanted_ver);

    if (!have_running) {
        for (trial = 3; trial != 0; --trial) {
            common_logf(SRC, 0x2ac, LOG_INFO, "Retry %d", trial);
            nanosleep_ms(US_poll_delay(s));
            have_running = vclient_get_version(US_vclient(s), &name,
                                               &running_ver);
            if (have_running) {
                break;
            }
        }
        if (!have_running) {
            manifest_entry_name(entry, &dev);
            common_logf(SRC, 0x2b3, LOG_WARN,
                "Unable to update device: %s, it did not report any version, "
                "trying to force update.", str_cstr(&dev));
            str_destroy(&dev);
        }
    }

    if (have_running) {
        if (!have_wanted) {
            /* OEM: throw -- the manifest entry has no target version. */
            str_destroy(&name);
            return -1;
        }
        if (wanted_ver == running_ver) {
            manifest_entry_name(entry, &dev);
            common_logf(SRC, 700, LOG_INFO,
                "No update required for: %s (current: %s, wanted %s).",
                str_cstr(&dev), version_str(running_ver),
                version_str(wanted_ver));
            str_destroy(&dev);
            str_destroy(&name);
            return 0;
        }
        manifest_entry_name(entry, &dev);
        common_logf(SRC, 0x2b7, LOG_INFO,
            "Handle update for: %s (current: %s, wanted %s).",
            str_cstr(&dev), version_str(running_ver), version_str(wanted_ver));
        str_destroy(&dev);
    }
    str_destroy(&name);

    /* flash: get a client and Update() it, up to 3 trials. */
    for (trial = 0; trial != 3; ++trial) {
        update_client *cli = factory_get_client(US_factory(s), entry); /* +0x10 */
        if (cli == NULL) {
            manifest_entry_name(entry, &dev);
            common_logf(SRC, 0x2c6, LOG_ERR,
                        "Could not get an update client for: %s.",
                        str_cstr(&dev));
            str_destroy(&dev);
            return -1;
        }
        if (update_client_update(cli) == 0) {               /* vtable +0x10 */
            common_logf(SRC, 0x2cb, LOG_INFO, "Update performed.");
            update_client_release(cli);
            return 0;
        }
        common_logf(SRC, 0x2ce, LOG_WARN,
                    "Update was not performed, trial : %d ", trial);
        update_client_release(cli);
    }
    return -1;
}

/* ================================================================== *
 *  wait_for_devices                                 OEM 0x00124500
 *
 *  Subscribes to "device/+/status", then polls the background pending count
 *  (US_pending_count, guarded by US_bg_mutex) once a second for up to 60s,
 *  logging "Waiting for %zu devices" each tick. Returns 0 when the pending
 *  count reaches 0, -1 on the 60s timeout. Unsubscribes on exit.
 * ================================================================== */
static int update_service_wait_for_devices(update_service *s)
{
    uint64_t start, elapsed;
    int rc;

    /* subscribe device/+/status -> on_device_status (FUN_00125960). */
    mqtt_subscribe(US_mqtt(s), "device/+/status",
                   (mqtt_handler)on_device_status, s);      /* vtable +0x10 */

    start = monotonic_ns();
    for (;;) {
        elapsed = monotonic_ns() - start;
        if (elapsed > 59999999999ULL) {                     /* 60 s window */
            rc = -1;
            break;
        }
        bg_mutex_lock(US_bg_mutex(s));
        if (US_pending_count(s) == 0) {
            bg_mutex_unlock(US_bg_mutex(s));
            rc = 0;
            break;
        }
        common_logf(SRC, 0x295, LOG_INFO, "Waiting for %zu devices",
                    US_pending_count(s));
        bg_mutex_unlock(US_bg_mutex(s));
        nanosleep_ms(1000);
    }

    mqtt_unsubscribe(US_mqtt(s), "device/+/status");        /* vtable +0x18 */
    return rc;
}

/* ================================================================== *
 *  run_device_updates                               OEM 0x00129c90
 *
 *  Walk the manifest device vector (US_manifest +8 begin / +0x10 end).
 *  `rolling_back` selects the skip rules:
 *    - rolling_back && entry.skip_rollback (+0x59): "Rolling back, skipping %s"
 *      and publish progress(step, SKIPPED).
 *    - charger devices are skipped during an ordinary (forward) update.
 *    - battery_primary_panasonic is only updated when the version client's
 *      "battery_primary" supplier byte is 'P' or 'f' (Panasonic); else skipped.
 *    - battery_primary_dynapack is only updated when that byte is NOT 'D'
 *      (Dynapack); i.e. it is the fallback supplier -- else skipped.
 *  For an updatable device:
 *    - publish "update/stage2/device_update_started" with the name (qos1).
 *    - handle_device_update():
 *        ok   -> progress(step, OK); if the device belongs to the
 *                "charger"/"_liteon_*" family enqueue it into the background
 *                update table (US_bg_table, guarded by US_bg_mutex).
 *        fail -> if entry.allowed_to_skip (+0x58):
 *                   "Failed updating device: %s , but it is allowed to be
 *                    skipped", progress(step, SKIPPED), and -- for charger/
 *                   modem devices -- publish a "start/trigger" background
 *                   update message to "update/background_update/<name>".
 *                else:
 *                   "Failed updating device: %s", progress(step, FAILED,err 13);
 *                   when not rolling back, abort the whole run with the device
 *                   return code.
 *  After the loop: "Waiting for devices to come online after update" ->
 *  wait_for_devices(); a timeout logs the "but I dont know what to do" warning.
 *  Returns 0 normally; the failing device's code when a non-skippable forward
 *  update fails.
 *  `step` is seeded at 10 (9 fixed steps + the first device is step 10).
 * ================================================================== */
int update_service_run_device_updates(update_service *s, char rolling_back)
{
    const manifest_entry *it  = manifest_device_begin(US_manifest(s));
    const manifest_entry *end = manifest_device_end(US_manifest(s));
    int step = 9;
    int rc   = 0;

    for (; it != end; it = manifest_device_next(it)) {
        str_t name;
        manifest_entry_name(it, &name);
        ++step;

        /* --- rollback skip: entry.skip_rollback (+0x59) ------------------ */
        if (rolling_back && manifest_entry_skip_rollback(it)) {
            common_logf(SRC, 0x22b, LOG_INFO, "Rolling back, skipping %s",
                        str_cstr(&name));
            publish_progress(s, (unsigned)step, -3, 0, &name);  /* SKIPPED */
            str_destroy(&name);
            continue;
        }

        /* --- charger: skip during an ordinary (forward) update ----------- */
        if (str_find(&name, TOK_CHARGER) != (size_t)-1) {
            common_logf(SRC, 0x232, LOG_INFO, "skip %s during ordinary update",
                        str_cstr(&name));
            publish_progress(s, (unsigned)step, -3, 0, &name);  /* SKIPPED */
            str_destroy(&name);
            continue;
        }

        /* --- battery_primary supplier gate (version-client byte) --------- */
        if (str_find(&name, TOK_BATTERY_PRIMARY_PANASONIC) != (size_t)-1) {
            char sup = vclient_get_supplier(US_vclient(s), TOK_BATTERY_PRIMARY);
            if (sup != 'P' && sup != 'f') {           /* not Panasonic -> skip */
                publish_progress(s, (unsigned)step, -3, 0, &name);
                str_destroy(&name);
                continue;
            }
        } else if (str_find(&name, TOK_BATTERY_PRIMARY_DYNAPACK) != (size_t)-1) {
            char sup = vclient_get_supplier(US_vclient(s), TOK_BATTERY_PRIMARY);
            if (sup == 'D') {                          /* is Dynapack -> skip */
                publish_progress(s, (unsigned)step, -3, 0, &name);
                str_destroy(&name);
                continue;
            }
        }

        /* --- update this device ----------------------------------------- */
        common_logf(SRC, 0x248, LOG_INFO, "Updating: %s.", str_cstr(&name));
        {
            json_t *started = json_string_new(str_cstr(&name));
            mqtt_publish_json(US_mqtt(s), "update/stage2/device_update_started",
                              started, 1, 0);
            json_release(started);
        }

        rc = update_service_handle_device_update(s, it);
        if (rc == 0) {
            publish_progress(s, (unsigned)step, 0, 0, &name);   /* OK */
            /* charger / _liteon_* family -> background update table. */
            if (name_in_charger_family(&name)) {
                bg_mutex_lock(US_bg_mutex(s));
                bg_table_add(US_bg_table(s), &name);            /* FUN_0012d100 */
                bg_mutex_unlock(US_bg_mutex(s));
            }
        } else {
            if (manifest_entry_allowed_to_skip(it)) {           /* +0x58 */
                common_logf(SRC, 0x25a, LOG_WARN,
                    "Failed updating device: %s , but it is allowed to be "
                    "skipped", str_cstr(&name));
                publish_progress(s, (unsigned)step, -3, 0, &name); /* SKIPPED */
                /* charger / modem device -> trigger a background update. */
                if (name_in_modem_family(&name)) {
                    common_logf(SRC, 0x25f, LOG_INFO,
                        "Publish a message to add %s device to background "
                        "update table", str_cstr(&name));
                    publish_background_update_trigger(s, &name);
                }
                rc = 0;
            } else {
                common_logf(SRC, 0x250, LOG_ERR,
                            "Failed updating device: %s", str_cstr(&name));
                publish_progress(s, (unsigned)step, -1, 13, &name); /* FAILED */
                if (!rolling_back) {
                    str_destroy(&name);
                    return rc;          /* abort the run with the device code */
                }
                rc = 0;
            }
        }
        str_destroy(&name);
    }

    common_logf(SRC, 0x276, LOG_INFO,
                "Waiting for devices to come online after update");
    if (update_service_wait_for_devices(s)) {
        common_logf(SRC, 0x278, LOG_ERR,
            "Devices have not all come back after update, but I dont know "
            "what to do");
    }
    return 0;
}

/* ================================================================== *
 *  stage2_install                                   OEM 0x0012a5d0
 *
 *  The "Stage 2" body run by run_state_machine: sleep US_stage2_delay, then
 *  run the forward device updates (run_device_updates(rolling_back=0)).
 *  Returns the device-update result (non-zero => stage-2 failed -> rollback).
 * ================================================================== */
static char stage2_install(update_service *s)
{
    nanosleep_ms(US_stage2_delay(s));
    return (char)update_service_run_device_updates(s, 0);
}

/* ================================================================== *
 *  run_state_machine                                OEM 0x0012a6d0
 *
 *  The top-level A/B update state machine, run on its own thread (started by
 *  update_service_to_updating_state @0x124210). Takes US_state_mutex(+0xd0).
 *
 *  1. sleep US_start_delay (+0xa8).
 *  2. stage = boot.get_stage()  (vtable +0x10)  -> US_stage(+0x140).
 *  3. US_total_steps(+0x130) = count_total_steps(stage).
 *  4. dispatch on stage:
 *
 *     stage 2  (A/B "Stage 2" install):
 *        progress(step9, status SKIPPED/0? -> OEM publishes step 9, status 0).
 *        run stage2_install():
 *          fail -> "Failed to install devices in update 'Stage 2', rolling
 *                   back." -> boot.rollback()(+0x78) -> boot.exec_reboot()(+0x88).
 *          ok   -> sleep US_poll_delay(+0xb0); boot.commit()(+0x70)
 *                  ("Failed to set commit to the new state." on error);
 *                  write_finished_flag(0); sleep US_reboot_delay(+0xb8);
 *                  boot.exec_reboot()(+0x88).
 *
 *     stage 1  (A/B "Stage 1"): boot.exec_reboot()(+0x88)
 *        ("Failed to set execute reboot after update 'Stage 1'." on error).
 *
 *     stage 3 or 4 (rollback): progress(step9, 0);
 *        run_device_updates(rolling_back=1)
 *          ("Failed to roll-back, unrecoverable." on error);
 *        boot.commit()(+0x70) ("Failed to set commit to the new state.");
 *        write_finished_flag(-1); boot.exec_reboot()(+0x88).
 *
 *     stage 0 / default (fresh FOTA install):
 *        if US_battery_soc(+0x290) < 0x0b:
 *            "Battery is to low to start update: %d";
 *            progress(0, status FAILED -1, err 2);
 *            state_client.request_state(US_prev_state(+0x134))  -- bail.
 *        else:
 *            progress(0, 0, 0);
 *            run_fota_install_sequence():
 *               fail -> "Failed to install()"; boot.commit()(+0x70)
 *                       ("Failed to restore state."); if boot.mount()(+0x90)
 *                       && boot.umount()(+0x58) -> "Unable to umount root";
 *                       write_finished_flag(-1); sleep US_reboot_delay(+0xb8);
 *                       boot.exec_reboot()(+0x88).
 *
 *  Releases US_state_mutex on exit.
 * ================================================================== */
void update_service_run_state_machine(update_service *s)
{
    signed char stage;
    str_t dev; str_init_empty(&dev);   /* &DAT_00171c20 == "" */

    state_mutex_lock(US_state_mutex(s));

    /* 1. settle */
    nanosleep_ms(US_start_delay(s));                        /* +0xa8 */

    /* 2-3. load stage + total steps */
    stage = (signed char)boot_get_stage(US_boot(s));        /* vtable +0x10 */
    US_stage(s)       = stage;                               /* +0x140 */
    US_total_steps(s) = count_total_steps(s, stage);        /* +0x130 */
    common_logf(SRC, 0xff, LOG_INFO, "TOTAL STEPS: %d", US_total_steps(s));

    if (stage == 2) {
        /* ----- A/B Stage 2: install the staged devices ----- */
        publish_progress(s, 9, 0, 0, &dev);
        if (stage2_install(s) != 0) {
            common_logf(SRC, 0x12a, LOG_ERR,
                "Failed to install devices in update 'Stage 2', rolling back.");
            if (boot_rollback(US_boot(s))) {                /* vtable +0x78 */
                common_logf(SRC, 300, LOG_ERR, "Failed to set rollback state.");
            }
            if (boot_exec_reboot(US_boot(s))) {             /* vtable +0x88 */
                common_logf(SRC, 0x12f, LOG_ERR,
                            "Failed to set execute reboot.");
            }
        } else {
            nanosleep_ms(US_poll_delay(s));                 /* +0xb0 */
            if (boot_commit(US_boot(s))) {                  /* vtable +0x70 */
                common_logf(SRC, 0x136, LOG_ERR,
                            "Failed to set commit to the new state.");
            }
            write_finished_flag(s, 0);
            nanosleep_ms(US_reboot_delay(s));               /* +0xb8 */
            if (boot_exec_reboot(US_boot(s))) {             /* vtable +0x88 */
                common_logf(SRC, 0x143, LOG_ERR,
                            "Failed to set execute reboot.");
            }
        }
    } else if (stage < 3) {
        if (stage == 1) {
            /* ----- A/B Stage 1: just reboot into the new slot ----- */
            if (boot_exec_reboot(US_boot(s))) {             /* vtable +0x88 */
                common_logf(SRC, 0x124, LOG_ERR,
                    "Failed to set execute reboot after update 'Stage 1'.");
            }
        } else {
            /* ----- stage 0: fresh FOTA install ----- */
            if (US_battery_soc(s) < 0x0b) {                 /* +0x290 < 11% */
                common_logf(SRC, 0x106, LOG_WARN,
                            "Battery is to low to start update: %d");
                publish_progress(s, 0, -1, 2, &dev);        /* FAILED */
                state_client_request_state(s, US_prev_state(s)); /* +0x134 */
            } else {
                publish_progress(s, 0, 0, 0, &dev);
                if (update_service_run_fota_install_sequence(s)) {
                    common_logf(SRC, 0x10e, LOG_ERR, "Failed to install()");
                    if (boot_commit(US_boot(s))) {          /* vtable +0x70 */
                        common_logf(SRC, 0x110, LOG_ERR,
                                    "Failed to restore state.");
                    }
                    if (boot_mount(US_boot(s)) &&           /* vtable +0x90 */
                        boot_umount(US_boot(s))) {          /* vtable +0x58 */
                        common_logf(SRC, 0x115, LOG_ERR,
                                    "Unable to umount root");
                    }
                    write_finished_flag(s, -1);
                    nanosleep_ms(US_reboot_delay(s));       /* +0xb8 */
                    if (boot_exec_reboot(US_boot(s))) {     /* vtable +0x88 */
                        common_logf(SRC, 0x11c, LOG_ERR,
                                    "Failed to set execute reboot.");
                    }
                }
            }
        }
    } else if (((stage - 3) & 0xff) < 2) {
        /* ----- stage 3 / 4: rollback ----- */
        publish_progress(s, 9, 0, 0, &dev);
        if (update_service_run_device_updates(s, 1)) {
            common_logf(SRC, 0x14b, LOG_ERR, "Failed to roll-back, unrecoverable.");
        }
        if (boot_commit(US_boot(s))) {                      /* vtable +0x70 */
            common_logf(SRC, 0x14f, LOG_ERR,
                        "Failed to set commit to the new state.");
        }
        write_finished_flag(s, -1);
        if (boot_exec_reboot(US_boot(s))) {                 /* vtable +0x88 */
            common_logf(SRC, 0x154, LOG_ERR,
                        "Failed to set execute reboot.");
        }
    }

    state_mutex_unlock(US_state_mutex(s));
    str_destroy(&dev);
}
