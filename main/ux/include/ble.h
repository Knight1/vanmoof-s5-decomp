/*
 * ble.h — reconstructed module decls for the VanMoof S5 `ux` service Ble glue
 * (program "ux", AArch64, image base 0x100000). Source file ble.cpp.
 *
 * The Ble object owns the per-connection state map and the BLE/connection MQTT
 * subscriptions. It is constructed with the UXService MQTT bus and tracks the
 * authenticated/touch-unlock state used by the unlock flow.
 *
 * Field offsets below mirror the OEM object layout used by the decompiled
 * accessors; only the fields the reconstructed functions touch are named.
 */
#ifndef UX_BLE_H
#define UX_BLE_H

#include "ux_common.h"

/* One entry in the per-connection RB-tree map (ux+0x80/0x88), keyed by the
 * connection id parsed out of the topic suffix. OEM node payload is
 * {int id (+0x20); int state (+0x24); uint8 user (+0x04 in info path)}. */
typedef struct ble_conn {
    int      id;          /* connection id (map key)            */
    int      state;       /* last authentication state (0/3/4)  */
    uint8_t  user;        /* reported user id (from info msg)    */
} ble_conn;

/* The Ble glue object (this == param_1 of the handlers; the UXService is
 * reachable through *param_1). Only the touch-unlock / authenticated flags and
 * the on-auth signal list are modelled here; the connection map and the MQTT
 * bus pointer are opaque. */
typedef struct ble_svc {
    ux_service *svc;          /* owning UXService (this[0xf])           */
    void       *conn_map;     /* RB-tree of ble_conn (this+0x80)        */
    void       *on_auth;      /* authenticated signal list (svc+0x38)   */
    bool        touch_unlock_enabled;   /* svc+0xb0 */
    bool        touch_unlock_active;    /* svc+0xb1 */
    bool        authenticated;          /* svc+0xb2 */
} ble_svc;

/* mqtt_handler-compatible entry points (registered by ble_ctor_subscribe). */
void ble_ctor_subscribe(ble_svc *self, mqtt_client *bus);
void ble_handle_connection_handle       (void *ctx, const char *topic, const json_t *payload);
void ble_handle_connection_authenticated(ble_svc *self, unsigned id, int state, const json_t *payload);
void ble_handle_connection_info         (void *ctx, const char *topic, const json_t *payload);
void ble_handle_touch_unlock_activate   (void *ctx, const char *topic, const json_t *payload);

/* Connection map helpers (RB-tree insert-or-find by id; std::map glue — VENDOR,
 * modelled at the call site only). */
ble_conn *ble_conn_map_find_or_insert(void *map, int id);
ble_conn *ble_conn_map_find          (void *map, int id);

/* Fire the authenticated/de-authenticated signal list (svc+0x38) for `id`.
 * std::function fan-out — VENDOR glue, modelled. */
void ble_emit_authenticated  (ble_svc *self, unsigned id, const char *user);
void ble_emit_deauthenticated(ble_svc *self, unsigned id);

#endif /* UX_BLE_H */
