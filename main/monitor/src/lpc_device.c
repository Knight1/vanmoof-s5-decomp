/*
 * lpc_device.c — VanMoof S5 i.MX8 `monitor` service: the LPC MotorController
 * IComponent (C++ class `15MotorController`). It is the supervisor's proxy for
 * the LPC54xxx motor-control sub-ECU (CAN node 0xA1 = "Motor"); the monitor
 * polls it like every other supervised component and re-publishes its identity
 * (version triplet) and health (status) to MQTT.
 *
 * Program "monitor", AArch64, image base 0x100000.
 *
 * GROUND TRUTH (decompiled, read-only pass):
 *   - The version/status publish path is FUN_0012c5c0 @0x12c5c0 (the
 *     MotorController setup/announce method). It builds the topic root with
 *     monitor_topic_root() (FUN_0012b390 @0x12b390), which calls the
 *     component's get_name() (vtable +0x30) and prepends the literal "device/"
 *     (root = "device/<name>"). It then appends, verbatim from .rodata:
 *       0x14f468 "/version/"  -> + "firmware"   (0x14d3e0)
 *                              -> + "bootloader" (0x14f478)
 *                              -> + "vendor"     (0x14f488)
 *       0x14f4a0 "/status"
 *     i.e. device/<name>/version/{firmware,bootloader,vendor} and
 *          device/<name>/status.
 *   - can_node_id_to_ecu_name() @0x12ceb0 maps 0xA1 -> "Motor".
 *
 * MODELLED (inlined / RELA-relocated, body not separately present):
 *   The individual IComponent override bodies and the supplier-suffix
 *   selection are inlined into the MotorController ctor / the supervisor loop
 *   and folded by -Os, so they have no standalone OEM address. They are
 *   reconstructed here as the documented IComponent overrides + the exact topic
 *   wiring proven above. Detail that is not visible is NOT fabricated.
 */
#include "monitor_common.h"
#include "lpc_modem.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Component identity. The version topics carry the name "motor_control"; the
 * health topic carries "motor" (matching device/motor/status). Both names are
 * fed through monitor_topic_root() = "device/" + name.                       */
#define LPC_VERSION_NAME  "motor_control"   /* -> device/motor_control/... */
#define LPC_STATUS_NAME   "motor"           /* -> device/motor/status      */
#define LPC_CAN_NODE      0xA1u             /* can_node_id_to_ecu_name -> "Motor" */
#define LPC_TYPE          "motor_controller"

/* Supplier suffix appended to the reported version string. The motor pack /
 * variant is identified over CAN and selects one of these tags (MODELLED:
 * the selection is inlined; the suffixes are the documented set). */
static const char *const k_lpc_supplier_suffix[] = {
    "_panasonic",
    "_dynapack",
    "_liteon_normal",
    "_liteon_speed",
};

const char *lpc_supplier_suffix(enum lpc_supplier s)
{
    if ((unsigned)s >= (sizeof k_lpc_supplier_suffix / sizeof k_lpc_supplier_suffix[0]))
        return "";
    return k_lpc_supplier_suffix[s];
}

/* ---- IComponent overrides (MODELLED) ----------------------------------- */

/* vt+0x30 get_name() — drives the topic root. */
static const char *lpc_get_name(icomponent *self)
{
    (void)self;
    return LPC_VERSION_NAME;
}

/* vt+0x38 get_status() — last reported health of the LPC node. */
static const char *lpc_get_status(icomponent *self)
{
    motor_controller *mc = (motor_controller *)self;
    return mc->alive ? "ok" : "error";
}

/* vt+0x40 get_version() — firmware version string read back over CAN,
 * with the supplier suffix appended (e.g. "1.8.0_liteon_normal"). */
static const char *lpc_get_version(icomponent *self)
{
    motor_controller *mc = (motor_controller *)self;
    return mc->version;
}

/* vt+0x48 is_alive() — true while the motor node answers on CAN 0xA1. */
static bool lpc_is_alive(icomponent *self)
{
    motor_controller *mc = (motor_controller *)self;
    return mc->alive;
}

/* vt+0x50 get_value() — seconds since the last CAN frame from the node. */
static float lpc_get_value(icomponent *self)
{
    motor_controller *mc = (motor_controller *)self;
    return mc->seconds_since_seen;
}

/* vt+0x58 get_type() */
static const char *lpc_get_type(icomponent *self)
{
    (void)self;
    return LPC_TYPE;
}

/*
 * lpc_publish_identity — the announce path, modelled on FUN_0012c5c0 @0x12c5c0.
 * Publishes the version triplet under device/<name>/version/... and the health
 * under device/<name>/status. Topic literals are reproduced verbatim from the
 * decompiled .rodata.
 */
static void lpc_publish_identity(motor_controller *mc)
{
    icomponent *self = &mc->base;
    mqtt_client *mq = self->mqtt;             /* param_1[10] in the OEM frame */
    char topic[128];
    const char *root = lpc_get_name(self);    /* vtable +0x30 (get_name) */

    /* device/<name>/status   ("/status" literal @0x14f4a0) */
    (void)snprintf(topic, sizeof topic, "device/%s/status", LPC_STATUS_NAME);
    mqtt_publish_str(mq, topic, lpc_get_status(self), 1, 0);

    /* device/<name>/version/firmware   ("/version/" @0x14f468 + "firmware") */
    (void)snprintf(topic, sizeof topic, "device/%s/version/firmware", root);
    mqtt_publish_str(mq, topic, lpc_get_version(self), 1, 0);

    /* device/<name>/version/bootloader (+ "bootloader" @0x14f478) */
    (void)snprintf(topic, sizeof topic, "device/%s/version/bootloader", root);
    mqtt_publish_str(mq, topic, mc->bootloader_version, 1, 0);

    /* device/<name>/version/vendor     (+ "vendor" @0x14f488) */
    (void)snprintf(topic, sizeof topic, "device/%s/version/vendor", root);
    mqtt_publish_str(mq, topic, mc->vendor, 1, 0);
}

/* vt poll() — per-tick supervise: refresh aliveness then re-announce. */
static void lpc_poll(icomponent *self)
{
    motor_controller *mc = (motor_controller *)self;

    if (!mc->alive)
        common_logf("motor_controller.cpp", 0, LOG_WARN,
                    "Motor controller (CAN %s) not responding",
                    can_node_id_to_ecu_name(LPC_CAN_NODE));
    lpc_publish_identity(mc);
}

/* MODELLED dtor — base teardown only (mosquitto sub-object is vendor). */
static void lpc_dtor(icomponent *self)
{
    icomponent_base_dtor(self);
}

static const icomponent_ops k_lpc_ops = {
    .dtor        = lpc_dtor,
    .get_name    = lpc_get_name,
    .get_status  = lpc_get_status,
    .get_version = lpc_get_version,
    .is_alive    = lpc_is_alive,
    .get_value   = lpc_get_value,
    .get_type    = lpc_get_type,
    .poll        = lpc_poll,
};

/*
 * motor_controller_ctor — MODELLED constructor. Installs the base IComponent
 * vtable + mosquitto handle (icomponent_base_ctor), overrides the vtable with
 * the MotorController ops, and announces the initial identity (FUN_0012c5c0).
 */
void motor_controller_ctor(motor_controller *mc, mqtt_client *mqtt,
                           enum lpc_supplier supplier)
{
    icomponent_base_ctor(&mc->base, mqtt);
    mc->base.ops          = &k_lpc_ops;
    mc->supplier          = supplier;
    mc->alive             = false;
    mc->seconds_since_seen = 0.0f;
    mc->version[0]        = '\0';
    mc->bootloader_version[0] = '\0';
    mc->vendor[0]         = '\0';

    lpc_publish_identity(mc);
}
