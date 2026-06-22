/*
 * update_service_client.c — reconstructed VanMoof S5 `ux` UpdateServiceClient.
 * OEM source: devices/main/ux/src/update_service_client.cpp.
 * Program "ux" (AArch64, base 0x100000).
 *
 * Behaviour-oriented: the json status extraction (FUN_00150af0 / FUN_0013c760),
 * the std::function signal list, and the MQTT publish glue are VENDOR and are
 * reached through modelled helpers. MQTT topics, the persisted byte fields, the
 * log string + line number are preserved verbatim.
 */
#include "ux_common.h"
#include "update_service_client.h"

#define KEY_STATUS "status"

/* -------------------------------------------------------------------------
 * update_client_ctor_subscribe — OEM 0x14fc90
 * Zero the object, install the vtable, store the bus at this[0xe], init the
 * stage/status bytes (stage = 0xff, have_status = 0), then register two MQTT
 * subscriptions:
 *   "update/ux/finished" -> update_client_handle_finished
 *   "update/stage"       -> update_client_handle_stage
 * ----------------------------------------------------------------------- */
void update_client_ctor_subscribe(update_client *self, mqtt_client *bus)
{
    self->bus = bus;
    self->on_finished = NULL;
    self->stage = 0xff;          /* this+0xf init in OEM = 0xff */
    self->status = 0;
    self->have_status = false;   /* this+0x7a = 0 */

    mqtt_subscribe(bus, "update/ux/finished",
                   update_client_handle_finished, self);

    mqtt_subscribe(bus, "update/stage",
                   update_client_handle_stage, self);
}

/* -------------------------------------------------------------------------
 * update_client_handle_finished — OEM 0x150050 (update_service_client.cpp:0x17)
 * Handler for "update/ux/finished". Reads the JSON "status" byte; stores it at
 * client+0x79, setting the "have status" flag (client+0x7a) on first receipt;
 * fires the finished signal list (client+0x38); then re-publishes
 * "update/finished" on the ux bus (this[0xe], vtable+0x30) to notify the rest
 * of ux. On a non-object payload / parse failure logs INFO
 * "Unable to extract status from update_finished msg %s".
 * ----------------------------------------------------------------------- */
void update_client_handle_finished(void *ctx, const char *topic,
                                   const json_t *payload)
{
    update_client *self = (update_client *)ctx;
    json_t status_j;
    int status;

    (void)topic;

    /* the OEM looks up obj["status"]; on a non-object payload it throws and the
     * catch path logs the INFO line below. */
    if (!json_find(payload, KEY_STATUS, &status_j) ||
        !json_get_int(&status_j, &status)) {
        char *dump = json_dump(payload);
        common_logf("devices/main/ux/src/update_service_client.cpp", 0x17,
                    LOG_INFO,
                    "Unable to extract status from update_finished msg %s",
                    dump);
        json_dump_free(dump);
        return;
    }

    /* persist the status; latch have_status on first receipt. */
    self->status = (uint8_t)status;
    if (!self->have_status)
        self->have_status = true;
    json_free(&status_j);

    /* fan out the finished signal list (client+0x38). */
    update_client_emit_finished(self, self->status);

    /* re-publish "update/finished" to the ux bus (this[0xe], vtable+0x30). */
    mqtt_publish_str(self->bus, "update/ux/finished", "", 1, 0);
}

/* -------------------------------------------------------------------------
 * update_client_handle_stage — OEM 0x14fff0
 * Handler for "update/stage". Parses the incoming JSON to a single byte
 * (FUN_00150af0) and stores it at client+0x78. Tracks the OTA stage/progress.
 * ----------------------------------------------------------------------- */
void update_client_handle_stage(void *ctx, const char *topic,
                                const json_t *payload)
{
    update_client *self = (update_client *)ctx;
    int stage = 0;

    (void)topic;

    json_get_int(payload, &stage);          /* FUN_00150af0 -> single byte */
    self->stage = (uint8_t)stage;           /* client+0x78 */
}
