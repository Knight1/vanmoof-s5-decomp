#include "update_common.h"
#include <pthread.h>

/* ===== module-local framework model (externs + structs) ===== */
typedef struct IMqttClient IMqttClient;
/* Modelled std::string (libstdc++ SSO layout, 0x20 bytes). Concrete so it can

   be embedded by value in version_info. */
typedef struct str_t {
    char  *ptr;        /* +0x00 data pointer (-> sso[] when small)            */
    size_t len;        /* +0x08 length                                        */
    union {            /* +0x10 capacity / 16-byte SSO buffer                 */
        size_t cap;
        char   sso[16];
    } u;
} str_t;

/* Per-device record stored as the std::map node payload. The rb-tree node

   header occupies the first 0x40 bytes of each node (color/parent/left/right

   at +0x10..+0x18, key std::string at +0x20); these three version strings are

   the mapped value, starting at node +0x40. Offsets are node-relative. */
typedef struct version_info {
    str_t bootloader;  /* node +0x40 : bootloader version  (-> u16)           */
    str_t firmware;    /* node +0x60 : firmware version    (-> u32)           */
    str_t vendor;      /* node +0x80 : vendor version      (-> u8)            */
} version_info;

/* Models std::map<std::string,version_info> (libstdc++ rb-tree, 0x30 bytes:

   header node + size). Opaque internals; access via vc_map_* helpers. */
typedef struct version_map {
    char _opaque[0x30];   /* _Rb_tree header @ object +0x40                   */
} version_map;

/* VersionClientMqtt — OEM object, size 0x90. */
typedef struct VersionClientMqtt {
    const void   *vtable;         /* +0x00  -> PTR_FUN_0019d0b8                */
    pthread_mutex_t mutex;        /* +0x08  guards 'devices' (0x28 bytes)     */
    void         *_mqtt_pad;      /* +0x30  (padding/alignment in OEM)        */
    IMqttClient  *mqtt;           /* +0x38  transport client (ctor arg)       */
    version_map   devices;        /* +0x40  device-name -> version_info       */
} VersionClientMqtt;

/* ---- modelled std::string (str_t is defined in structs[]) ---- */
void str_init(str_t *s);
void str_free(str_t *s);
size_t str_len(const str_t *s);
const char *str_data(const str_t *s);   /* raw buffer, may be non-NUL-terminated view */
const char *str_c(const str_t *s);      /* NUL-terminated C string for %s / logging */
void str_copy(str_t *dst, const str_t *src);
void str_assign(str_t *dst, const str_t *src);   /* std::string::_M_assign */
int  str_eq_c(const str_t *s, const char *lit);  /* std::string::compare == 0 */
/* split a std::string on a single char separator into out[]; returns count.
   Models FUN_00157fa0 (vector<std::string> split, vector entry = 0x20 bytes). */
size_t str_split_obj(const str_t *s, char sep, str_t out[], size_t max);
/* strtol wrapper FUN_00119b50: parses str in given base, range-checked to
   int32; throws std::out_of_range/std::invalid_argument ("stoi") on failure. */
long str_to_long(const str_t *s, int base);

/* ---- C++ runtime error helpers (modelled, never returns) ---- */
void throw_invalid_argument(const char *what);
void throw_out_of_range_fmt(const char *fmt, size_t a, size_t b);

/* ---- mutex (pthread, modelled opaque) ---- */
void vc_mutex_init(pthread_mutex_t *m);
void vc_mutex_lock(pthread_mutex_t *m);    /* throws std::system_error on EINVAL */
void vc_mutex_unlock(pthread_mutex_t *m);

/* ---- device -> version_info map (models std::map<std::string,version_info>) ---- */
/* vc_map_find  models FUN_0012f480: returns node payload ptr or NULL if absent. */
version_info *vc_map_find(version_map *m, const str_t *key);
/* vc_map_get_or_insert models map::operator[] (FUN_0012f9e0): finds the node for
   key, default-constructing one (all version_info strings "") if absent. */
version_info *vc_map_get_or_insert(version_map *m, const str_t *key);
void vc_map_init(version_map *m);
void vc_map_free(version_map *m);

/* ---- MQTT transport (IMqttClient, modelled opaque) ---- */
typedef struct IMqttClient IMqttClient;
typedef struct mqtt_msg mqtt_msg;   /* nlohmann::json-typed message */
typedef void (*mqtt_cb_t)(void *ctx, void *a, const str_t *topic, const mqtt_msg *msg);
/* subscribe via vtable+0x10; QoS is the trailing int (always 1 here). */
void mqtt_subscribe(IMqttClient *c, const char *topic, mqtt_cb_t cb, void *ctx, int qos);
/* unsubscribe via vtable+0x18. */
void mqtt_unsubscribe(IMqttClient *c, const char *topic);
/* FUN_001178f0: require the json message be a string and copy it out; throws
   "type must be string, but is <type>" otherwise. */
void mqtt_msg_to_string(const mqtt_msg *msg, str_t *out);

/* VersionClientMqtt vtable @0x0019d0b8 (slots: ~dtor, deleting-dtor,
   get_device_version_u32, get_device_field_u16, get_device_field_u8). */
extern const void *const PTR_FUN_0019d0b8;
/* =========================================================== */

/*
 * version_client_mqtt.c — VanMoof OTA "update" service
 *
 * VersionClientMqtt: a per-device reported-version store. It subscribes to the
 * MQTT topics
 *      device/+/version/firmware/#
 *      device/+/version/bootloader/#
 *      device/+/version/vendor/#
 * and, for every retained/published message, records the (string) version of
 * the named device into an in-memory std::map<device_name, version_info>,
 * guarded by a mutex. Three getters then look a device up by name and parse the
 * stored version string into a uint32_t / uint16_t / uint8_t.
 *
 * Source: devices/main/update/src/version_client_mqtt.cpp
 *
 * OEM object layout (see VersionClientMqtt struct):
 *   +0x08  pthread_mutex_t       mutex
 *   +0x38  IMqttClient*          mqtt          (transport, ctor arg)
 *   +0x40  version_map           devices       (std::map node header at +0x40)
 *
 * Per-device version_info node payload (see version_info struct):
 *   +0x40  std::string  bootloader   (-> get_device_field_u16)
 *   +0x60  std::string  firmware     (-> get_device_version_u32)
 *   +0x80  std::string  vendor       (-> get_device_field_u8)
 *
 * The std::map / std::string framework is MODELLED here (vc_map_*, str_t),
 * not rebuilt. This TU compiles standalone for behavioural review.
 */
/* ------------------------------------------------------------------ */
/* version string parsers                                             */
/* ------------------------------------------------------------------ */

/*
 * FUN_00159900 — decode the firmware build qualifier into 0..3.
 *   "dev"  -> 1
 *   "rc"   -> 2
 *   "main" -> 3
 *   ""     -> 3   (empty)
 *   else   -> 0
 * Strings @0x177b18/0x177b20/0x177b28.
 */
static uint32_t version_parse_qualifier(const str_t *s) /* @0x00159900 */
{
    if (str_eq_c(s, "dev"))
        return 1;
    if (str_eq_c(s, "rc"))
        return 2;
    if (str_eq_c(s, "main"))
        return 3;
    /* empty -> 3, any other non-empty -> 0 */
    return (str_len(s) != 0) ? 0 : 3;
}

/*
 * FUN_0015c0d0 — parse a firmware version string into a packed uint32_t.
 *
 * Format: "major.minor.patch[-qualifier]"
 * Steps (verbatim from the machine code):
 *   1. split on '-'  (0x2d). If exactly 2 parts (vector byte-size == 0x40),
 *      decode parts[1] via version_parse_qualifier(); result masked & 0xff.
 *      Otherwise qualifier defaults to 3.
 *   2. split parts[0] on '.'  (0x2e). Need >= 3 parts (byte-size >= 0x60),
 *      else throw std::invalid_argument(
 *        "Error, Firmware format should be : major.minor.patch").
 *   3. major = strtol(parts[0],10); minor = strtol(parts[1],10);
 *      patch = strtol(parts[2],10).
 *   4. u32 = (major<<24) | (minor<<16) | patch | (qualifier<<13).
 *
 * Returns 0 if the input string is empty.
 */
static uint32_t version_string_to_u32(const str_t *ver) /* @0x0015c0d0 */
{
    str_t parts[8];
    size_t n;
    uint32_t qualifier = 3;
    uint32_t major, minor, patch;

    if (str_len(ver) == 0)
        return 0;

    /* split on '-' : "x.y.z" and optional "qualifier" */
    n = str_split_obj(ver, '-', parts, 8);
    if (n == 2)                                    /* (size == 0x40) */
        qualifier = version_parse_qualifier(&parts[1]) & 0xff;

    /* split the first part on '.' */
    {
        str_t dots[8];
        size_t m = str_split_obj(&parts[0], '.', dots, 8);
        if (m < 3) {                               /* (size < 0x60) */
            throw_invalid_argument(
                "Error, Firmware format should be : major.minor.patch");
            return 0; /* not reached */
        }
        major = (uint32_t)str_to_long(&dots[0], 10); /* FUN_00119b50 / strtol */
        minor = (uint32_t)str_to_long(&dots[1], 10);
        patch = (uint32_t)str_to_long(&dots[2], 10);
    }

    return (major << 24) | (minor << 16) | patch | (qualifier << 13);
}

/*
 * FUN_0015bf40 — parse a bootloader version "major.minor[...]" into uint16_t.
 * Split on '.' (0x2e); need >= 2 parts (byte-size >= 0x40).
 *   u16 = ((major & 0xff) << 8) | minor, masked & 0xffff.
 * Returns 0 if input empty or fewer than 2 parts.
 */
static uint16_t version_string_to_u16(const str_t *ver) /* @0x0015bf40 */
{
    str_t parts[8];
    size_t n;
    uint16_t major, minor;

    if (str_len(ver) == 0)
        return 0;

    n = str_split_obj(ver, '.', parts, 8);
    if (n < 2)                                     /* (size < 0x40) */
        return 0;

    major = (uint16_t)str_to_long(&parts[0], 10);
    minor = (uint16_t)str_to_long(&parts[1], 10);
    return (uint16_t)(((major & 0xff) << 8) | minor);
}

/*
 * FUN_00159d20 — "parse" a vendor version into uint8_t: first byte of the
 * string, or 0 if empty.
 */
static uint8_t version_string_to_u8(const str_t *ver) /* @0x00159d20 */
{
    if (str_len(ver) == 0)
        return 0;
    return (uint8_t)str_data(ver)[0];
}

/* ------------------------------------------------------------------ */
/* getters                                                            */
/* ------------------------------------------------------------------ */

/*
 * version_client_get_device_version_u32 — @0x0012e350
 *
 * out (param_2) is the device-name std::string key.
 * Locks the mutex, looks the device up in the map. If absent, logs a warning
 * and returns false. If present, parses the stored firmware string (node +0x60)
 * into a uint32_t. Empty firmware string -> false. On success logs the value
 * and returns true.
 *
 * Returns a {uint32_t value; bool ok;} pair by value in the OEM (here via out).
 */
bool version_client_get_device_version_u32(VersionClientMqtt *self,
                                           const str_t *device,
                                           uint32_t *out_version) /* @0x0012e350 */
{
    version_info *node;
    bool ok = false;
    uint32_t value = 0;

    vc_mutex_lock(&self->mutex);

    node = vc_map_find(&self->devices, device);     /* FUN_0012f480 */
    if (node == NULL) {
        common_logf("devices/main/update/src/version_client_mqtt.cpp", 0x73,
                    LOG_WARN, "No reported device versions for: %s",
                    str_c(device));
        ok = false;
    } else {
        /* map::at() semantics: node found means key present */
        if (str_len(&node->firmware) == 0) {
            ok = false;
        } else {
            value = version_string_to_u32(&node->firmware);  /* FUN_0015c0d0 */
            common_logf("devices/main/update/src/version_client_mqtt.cpp",
                        0x69, LOG_INFO,
                        "%s Version : uint32_t:%8x - string:%s",
                        str_c(device), value, str_c(&node->firmware));
            ok = true;
        }
    }

    vc_mutex_unlock(&self->mutex);
    if (out_version)
        *out_version = value;
    return ok;
}

/*
 * version_client_get_device_field_u16 — @0x0012e670
 *
 * Looks the device up; if absent logs a warning and returns 0. Otherwise parses
 * the stored bootloader string (node +0x40) into a uint16_t (0 if empty).
 */
uint16_t version_client_get_device_field_u16(VersionClientMqtt *self,
                                             const str_t *device) /* @0x0012e670 */
{
    version_info *node;
    uint16_t result = 0;

    vc_mutex_lock(&self->mutex);

    node = vc_map_find(&self->devices, device);     /* FUN_0012f480 */
    if (node == NULL) {
        common_logf("devices/main/update/src/version_client_mqtt.cpp", 0x81,
                    LOG_WARN,
                    "Device name not found reported device versions: %s",
                    str_c(device));
        result = 0;
    } else {
        result = version_string_to_u16(&node->bootloader); /* FUN_0015bf40 */
    }

    vc_mutex_unlock(&self->mutex);
    return result;
}

/*
 * version_client_get_device_field_u8 — @0x0012e8f0
 *
 * Looks the device up; if absent logs a warning and returns 0. Otherwise
 * returns the first byte of the stored vendor string (node +0x80), 0 if empty.
 */
uint8_t version_client_get_device_field_u8(VersionClientMqtt *self,
                                           const str_t *device) /* @0x0012e8f0 */
{
    version_info *node;
    uint8_t result = 0;

    vc_mutex_lock(&self->mutex);

    node = vc_map_find(&self->devices, device);     /* FUN_0012f480 */
    if (node == NULL) {
        common_logf("devices/main/update/src/version_client_mqtt.cpp", 0x8e,
                    LOG_WARN,
                    "Device name not found reported device versions: %s",
                    str_c(device));
        result = 0;
    } else {
        result = version_string_to_u8(&node->vendor);   /* FUN_00159d20 */
    }

    vc_mutex_unlock(&self->mutex);
    return result;
}

/* ------------------------------------------------------------------ */
/* topic -> device-name helper                                        */
/* ------------------------------------------------------------------ */

/*
 * FUN_0012e160 — extract the device name from a "device/<NAME>/version/<kind>"
 * topic by splitting on '/' (0x2f) and returning element [1]. Throws
 * std::out_of_range if there are fewer than 2 segments (vector byte-size 0x40).
 */
static void version_topic_device_name(const str_t *topic, str_t *out) /* @0x0012e160 */
{
    str_t segs[16];
    size_t n = str_split_obj(topic, '/', segs, 16);
    if (n < 2) {                                   /* (size < 0x40) */
        throw_out_of_range_fmt(
            "vector::_M_range_check: __n (which is %zu) >= this->size() (which is %zu)",
            (size_t)1, n);
        return; /* not reached */
    }
    str_copy(out, &segs[1]);
}

/* ------------------------------------------------------------------ */
/* subscription handlers (map population)                             */
/* ------------------------------------------------------------------ */

/*
 * Shared body for the three subscribe callbacks. The MQTT layer delivers
 * (topic, message); the message must be a JSON string (FUN_001178f0 converts
 * it, throwing "type must be string, but is <type>" otherwise). The handler:
 *   1. locks the mutex,
 *   2. extracts the device name from the topic,
 *   3. map::operator[](name)  (insert-or-find, FUN_0012f9e0; default value ""),
 *   4. assigns the payload into the requested node field,
 *   5. unlocks.
 * field_off selects which std::string in the node to write:
 *   +0x40 bootloader, +0x60 firmware, +0x80 vendor.
 */
static void version_store_field(VersionClientMqtt *self,
                                const str_t *topic,
                                const str_t *payload,
                                size_t field_off)
{
    str_t name;
    version_info *node;

    vc_mutex_lock(&self->mutex);

    version_topic_device_name(topic, &name);            /* FUN_0012e160 */
    node = vc_map_get_or_insert(&self->devices, &name); /* FUN_0012f9e0, "" default */

    str_assign((str_t *)((char *)node + field_off), payload);

    str_free(&name);
    vc_mutex_unlock(&self->mutex);
}

/*
 * FUN_0012eb70 — firmware version message handler.
 * Stores the payload into node +0x60 (firmware string).
 */
static void version_on_firmware(VersionClientMqtt *self, const str_t *topic,
                                const str_t *payload) /* @0x0012eb70 */
{
    version_store_field(self, topic, payload, 0x60);
}

/*
 * FUN_0012ef90 — bootloader version message handler.
 * Stores the payload into node +0x40 (bootloader string).
 */
static void version_on_bootloader(VersionClientMqtt *self, const str_t *topic,
                                  const str_t *payload) /* @0x0012ef90 */
{
    version_store_field(self, topic, payload, 0x40);
}

/*
 * FUN_0012ed80 — vendor version message handler.
 * Stores the payload into node +0x80 (vendor string).
 */
static void version_on_vendor(VersionClientMqtt *self, const str_t *topic,
                              const str_t *payload) /* @0x0012ed80 */
{
    version_store_field(self, topic, payload, 0x80);
}

/*
 * std::function trampolines installed in the subscribe calls. The MQTT delivery
 * callback signature is (void *ctx, ?, const str_t *topic, const mqtt_msg *).
 * FUN_001178f0 turns the JSON message into a std::string before dispatch.
 *   firmware:   FUN_0012f1a0 -> FUN_0012eb70
 *   bootloader: FUN_0012f260 -> FUN_0012ef90
 *   vendor:     FUN_0012f320 -> FUN_0012ed80
 */
static void version_cb_firmware(VersionClientMqtt **ctx, void *unused,
                                const str_t *topic,
                                const mqtt_msg *msg) /* @0x0012f1a0 */
{
    str_t payload;
    (void)unused;
    str_init(&payload);
    mqtt_msg_to_string(msg, &payload);              /* FUN_001178f0 */
    version_on_firmware(*ctx, topic, &payload);
    str_free(&payload);
}

static void version_cb_bootloader(VersionClientMqtt **ctx, void *unused,
                                  const str_t *topic,
                                  const mqtt_msg *msg) /* @0x0012f260 */
{
    str_t payload;
    (void)unused;
    str_init(&payload);
    mqtt_msg_to_string(msg, &payload);
    version_on_bootloader(*ctx, topic, &payload);
    str_free(&payload);
}

static void version_cb_vendor(VersionClientMqtt **ctx, void *unused,
                              const str_t *topic,
                              const mqtt_msg *msg) /* @0x0012f320 */
{
    str_t payload;
    (void)unused;
    str_init(&payload);
    mqtt_msg_to_string(msg, &payload);
    version_on_vendor(*ctx, topic, &payload);
    str_free(&payload);
}

/* ------------------------------------------------------------------ */
/* ctor / dtor                                                        */
/* ------------------------------------------------------------------ */

/*
 * VersionClientMqtt::VersionClientMqtt — @0x0012dba0
 *
 * Stores the IMqttClient (mqtt), zeroes the device map, then subscribes to the
 * three retained version topics, binding the corresponding handler to each.
 * The subscribe vtable slot is mqtt->vtable+0x10; QoS arg = 1.
 */
void version_client_mqtt_ctor(VersionClientMqtt *self,
                              IMqttClient *mqtt) /* @0x0012dba0 */
{
    self->vtable = &PTR_FUN_0019d0b8;     /* VersionClientMqtt vtable */
    vc_map_init(&self->devices);
    vc_mutex_init(&self->mutex);
    self->mqtt = mqtt;

    /* device/+/version/firmware/#  (len 0x1b) */
    mqtt_subscribe(mqtt, "device/+/version/firmware/#",
                   (mqtt_cb_t)version_cb_firmware, self, 1);
    /* device/+/version/bootloader/#  (len 0x1d) */
    mqtt_subscribe(mqtt, "device/+/version/bootloader/#",
                   (mqtt_cb_t)version_cb_bootloader, self, 1);
    /* device/+/version/vendor/#  (len 0x19) */
    mqtt_subscribe(mqtt, "device/+/version/vendor/#",
                   (mqtt_cb_t)version_cb_vendor, self, 1);
}

/*
 * VersionClientMqtt::~VersionClientMqtt — @0x0012df20
 *
 * Unsubscribes the three topics (mqtt->vtable+0x18 = unsubscribe), frees the
 * map and its nodes. (Slot 1 of the vtable, FUN_0012e130, is the deleting dtor:
 * this dtor then operator_delete(self, 0x90).)
 */
void version_client_mqtt_dtor(VersionClientMqtt *self) /* @0x0012df20 */
{
    self->vtable = &PTR_FUN_0019d0b8;
    mqtt_unsubscribe(self->mqtt, "device/+/version/firmware/#");
    mqtt_unsubscribe(self->mqtt, "device/+/version/bootloader/#");
    mqtt_unsubscribe(self->mqtt, "device/+/version/vendor/#");
    vc_map_free(&self->devices);
}