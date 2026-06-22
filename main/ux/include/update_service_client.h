/*
 * update_service_client.h — reconstructed module decls for the VanMoof S5 `ux`
 * service UpdateServiceClient (program "ux", AArch64, base 0x100000).
 * Source file update_service_client.cpp.
 *
 * The client subscribes to the separate update service over MQTT and mirrors
 * its progress/finished status into the ux bus. Field offsets mirror the OEM
 * object: bus pointer at this[0xe], stage byte at +0x78, status byte at +0x79,
 * "have status" flag at +0x7a, signal list at +0x38.
 */
#ifndef UX_UPDATE_SERVICE_CLIENT_H
#define UX_UPDATE_SERVICE_CLIENT_H

#include "ux_common.h"

typedef struct update_client {
    mqtt_client *bus;          /* this[0xe] — the ux MQTT bus            */
    void        *on_finished;  /* finished signal list (this+0x38)      */
    uint8_t      stage;        /* this+0x78 — last reported OTA stage    */
    uint8_t      status;       /* this+0x79 — last finished status       */
    bool         have_status;  /* this+0x7a — status received at least once */
} update_client;

/* OEM 0x14fc90: ctor — register the two update-service subscriptions. */
void update_client_ctor_subscribe(update_client *self, mqtt_client *bus);

/* mqtt_handler-compatible entry points. */
void update_client_handle_finished(void *ctx, const char *topic, const json_t *payload);
void update_client_handle_stage   (void *ctx, const char *topic, const json_t *payload);

/* Fire the finished signal list (this+0x38) — std::function fan-out, VENDOR. */
void update_client_emit_finished(update_client *self, uint8_t status);

#endif /* UX_UPDATE_SERVICE_CLIENT_H */
