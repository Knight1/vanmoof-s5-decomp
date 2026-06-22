/*
 * ble.c — reconstructed VanMoof S5 `ux` service Ble glue.
 * OEM source: devices/main/ux/src/ble.cpp.  Program "ux" (AArch64, base 0x100000).
 *
 * Behaviour-oriented reconstruction: the nlohmann::json operator[] machinery,
 * the std::map RB-tree, and the std::function signal lists are VENDOR and are
 * modelled through helper call-sites (declared in ble.h). The control flow,
 * MQTT topics, JSON keys, log strings + line numbers, and state semantics are
 * preserved verbatim from the decompiler output.
 */
#include "ux_common.h"
#include "ble.h"

#include <stdlib.h>   /* strtol */
#include <string.h>   /* strrchr */

/* JSON object keys / MQTT topics seen in the image (rodata literals). */
#define KEY_STATE   "state"          /* DAT_001c5cc8 */
#define KEY_USER    "u"             /* DAT_001c5c70 (OEM probes "o" then reads value key "u") */
#define KEY_CID     "cid"            /* DAT_001c5c08 */
#define KEY_RSSI    "rssi"           /* DAT_001c5c10 */
#define KEY_ENABLED "enabled"
#define KEY_ACTIVATED "activated"

/* connection state enum values carried in the "state" field:
 *  3 = authenticated, 0 = connected, 4 = disconnected (mask 0xfffffffb == 0
 *  catches {0,4}). */
enum ble_conn_state {
    BLE_CS_CONNECTED     = 0,
    BLE_CS_AUTHENTICATED = 3,
    BLE_CS_DISCONNECTED  = 4
};

/* -------------------------------------------------------------------------
 * ble_ctor_subscribe — OEM 0x172290
 * UpdateServiceClient-style ctor: zero the object, install the vtable, then
 * register three MQTT subscriptions on the bus (this[0xf]):
 *   "ble/connections/handle/+" -> ble_handle_connection_handle (per-conn wildcard)
 *   "ble/connections/info"     -> ble_handle_connection_info
 *   "ux/lock/touch_unlock_activate" -> ble_handle_touch_unlock_activate
 * ----------------------------------------------------------------------- */
void ble_ctor_subscribe(ble_svc *self, mqtt_client *bus)
{
    self->svc = NULL;
    self->conn_map = NULL;
    self->on_auth = NULL;
    self->touch_unlock_enabled = false;
    self->touch_unlock_active = false;
    self->authenticated = false;

    /* this[0xf] = bus (the IMQTTClient passed in) */
    /* build "ble/connections/handle" + "/+" -> "ble/connections/handle/+" */
    mqtt_subscribe(bus, "ble/connections/handle/+",
                   ble_handle_connection_handle, self);

    mqtt_subscribe(bus, "ble/connections/info",
                   ble_handle_connection_info, self);

    mqtt_subscribe(bus, "ux/lock/touch_unlock_activate",
                   ble_handle_touch_unlock_activate, self);
}

/* -------------------------------------------------------------------------
 * ble_handle_connection_handle — OEM 0x174d00
 * Handler for "ble/connections/handle/{id}". Parses the connection id from the
 * topic suffix (substring after the last '/'), reads the JSON "state" key (a
 * string mapped to an enum), then looks up / inserts the connection entry in
 * the per-connection map (ux+0x88) keyed by id and dispatches to
 * ble_handle_connection_authenticated. Throws std::invalid_argument on a
 * non-numeric suffix (DAT_001c5cc0 == "stoi").
 * ----------------------------------------------------------------------- */
void ble_handle_connection_handle(void *ctx, const char *topic,
                                  const json_t *payload)
{
    ble_svc *self = (ble_svc *)ctx;
    const char *slash;
    long id;
    int state;
    json_t state_j;
    ble_conn *entry;

    /* substring after the last '/' in the topic, then std::stoi ("stoi"). */
    slash = strrchr(topic, '/');
    if (slash == NULL)
        return;
    id = strtol(slash + 1, NULL, 10);          /* may throw invalid_argument */
    id &= 0xff;                                 /* (uint)lVar7 & 0xff */

    /* Resolve the "state" string -> enum. Modelled: the OEM walks the json RB
     * tree for "state" and string-matches against
     * connected/authenticated/disconnected. state defaults to 0. */
    state = 0;
    if (json_find(payload, KEY_STATE, &state_j)) {
        const char *s = json_get_string(&state_j);
        if (s != NULL) {
            if (strcmp(s, "authenticated") == 0) state = BLE_CS_AUTHENTICATED;
            else if (strcmp(s, "connected")  == 0) state = BLE_CS_CONNECTED;
            else if (strcmp(s, "disconnected") == 0) state = BLE_CS_DISCONNECTED;
        }
        json_free(&state_j);
    }

    /* find-or-insert the connection node by id (ux+0x88 RB-tree), store the
     * latest state, and dispatch. */
    entry = ble_conn_map_find_or_insert(self->conn_map, (int)id);
    entry->state = state;
    ble_handle_connection_authenticated(self, (unsigned)id, state, payload);
}

/* -------------------------------------------------------------------------
 * ble_handle_connection_authenticated — OEM 0x173980
 * Per-connection authentication transition. Reads the JSON "user" string
 * (defaults to "default" / DAT_001b1250 when absent). If touch-unlock was
 * active it is cleared, logging WARN "Touch unlock deactivated" (ble.cpp:0x5d).
 * On state==3 (authenticated): logs WARN "Connection %d for user %s
 * authenticated" (ble.cpp:0x65), fires the authenticated signal list
 * (svc+0x38) and sets svc+0xb2=1. On state in {0,4}: fires the
 * de-authenticated signal list and clears svc+0xb2.
 * ----------------------------------------------------------------------- */
void ble_handle_connection_authenticated(ble_svc *self, unsigned id,
                                         int state, const json_t *payload)
{
    json_t user_j;
    const char *user = "default";   /* DAT_001b1250 default when key absent */

    if (json_find(payload, KEY_USER, &user_j)) {
        const char *s = json_get_string(&user_j);
        if (s != NULL)
            user = s;
    }

    /* clearing touch unlock if it was active (svc+0xb1) */
    if (self->touch_unlock_active) {
        common_logf("devices/main/ux/src/ble.cpp", 0x5d, LOG_WARN,
                    "Touch unlock deactivated");
    }
    self->touch_unlock_active = false;

    if (state == BLE_CS_AUTHENTICATED) {
        common_logf("devices/main/ux/src/ble.cpp", 0x65, LOG_WARN,
                    "Connection %d for user %s authenticated", id, user);
        ble_emit_authenticated(self, id, user);
        self->authenticated = true;
    } else if ((state & 0xfffffffb) == 0) {     /* state == 0 || state == 4 */
        ble_emit_deauthenticated(self, id);
        self->authenticated = false;
    }

    if (json_get_string(&user_j) == user && user != (const char *)0)
        json_free(&user_j);     /* release the json scratch (modelled) */
}

/* -------------------------------------------------------------------------
 * ble_handle_connection_info — OEM 0x1729c0
 * Handler for "ble/connections/info". Reads JSON "cid" (connection id) and the
 * "rssi"/user value; if the reported connection differs from the current one
 * (svc+0x70) it looks up the connection node in ux+0x80 and stores the user id
 * at entry+4. Maintains the connection -> user mapping.
 * ----------------------------------------------------------------------- */
void ble_handle_connection_info(void *ctx, const char *topic,
                                const json_t *payload)
{
    ble_svc *self = (ble_svc *)ctx;
    json_t cid_j, rssi_j;
    int cid;
    int user;
    ble_conn *entry;

    (void)topic;

    /* require both keys present and numeric (type in {5,6,7} == int/uint/float). */
    if (!json_find(payload, KEY_CID, &cid_j))
        return;
    if (!json_get_int(&cid_j, &cid)) { json_free(&cid_j); return; }

    if (!json_find(payload, KEY_RSSI, &rssi_j)) { json_free(&cid_j); return; }

    /* if the reported connection equals the current connection (svc+0x70) we
     * are looking at our own link — nothing to record. */
    /* current-connection compare is modelled by the find below: */
    if (!json_get_int(&rssi_j, &user)) {
        json_free(&cid_j); json_free(&rssi_j); return;
    }
    json_free(&cid_j);
    json_free(&rssi_j);

    /* store the user id at entry+4 for this connection (ux+0x80 lookup). */
    entry = ble_conn_map_find(self->conn_map, cid);
    if (entry != NULL)
        entry->user = (uint8_t)user;
}

/* -------------------------------------------------------------------------
 * ble_handle_touch_unlock_activate — OEM 0x173120 (ble.cpp:0x45)
 * Handler for "ux/lock/touch_unlock_activate". If obj["enabled"] is a boolean
 * that is true AND obj["enabled"]["activated"]-style resolution yields true,
 * stores the activation flag at svc+0xb0 and sets svc+0xb1=1; otherwise clears
 * svc+0xb1. Logs WARN "Touch unlock %s" with "activated"/"deactivated".
 * ----------------------------------------------------------------------- */
void ble_handle_touch_unlock_activate(void *ctx, const char *topic,
                                      const json_t *payload)
{
    ble_svc *self = (ble_svc *)ctx;
    json_t enabled_j;
    const char *word = "deactivated";
    bool enabled = false;

    (void)topic;

    /* obj["enabled"] must be a boolean (json type tag 4) and true. */
    if (json_find(payload, KEY_ENABLED, &enabled_j)) {
        if (json_get_bool(&enabled_j, &enabled) && enabled) {
            bool activated = false;
            json_t act_j;
            /* the OEM then re-reads the "rssi"/activation value to pull the
             * activation bool stored at svc+0xb0. */
            if (json_find(payload, KEY_RSSI, &act_j)) {
                json_get_bool(&act_j, &activated);
                json_free(&act_j);
            }
            self->touch_unlock_enabled = activated;   /* svc+0xb0 */
            self->touch_unlock_active  = true;        /* svc+0xb1 = 1 */
            word = "activated";
        } else {
            self->touch_unlock_active = false;        /* svc+0xb1 = 0 */
        }
        json_free(&enabled_j);
    } else {
        self->touch_unlock_active = false;
    }

    common_logf("devices/main/ux/src/ble.cpp", 0x45, LOG_WARN,
                "Touch unlock %s", word);
}
