/*
 * monitor_service.c — VanMoof S5 i.MX8 `monitor` MonitorService.
 *
 * The supervisor owns a vector of IComponent subclasses (BLE, LPC
 * MotorController, modem) and, every poll tick, drives each through the
 * IComponent vtable to gather name/status/version/alive/value/type, then (in
 * `service` mode) repaints a single-screen ANSI status table and, otherwise,
 * publishes/aggregates. It also registers a CANopen heartbeat callback so it
 * can flag heartbeats from devices it did not expect.
 *
 * Program "monitor", AArch64, image base 0x100000. STL / mosquitto / CANopen
 * glue is modelled at the call site (vendor); the VanMoof control flow, log
 * strings + lines + levels, and the IComponent slot semantics are reproduced.
 */
#include "monitor_service.h"
#include <stdio.h>
#include <string.h>

/* registry vector: begin/end are stored at raw word indices 0x13/0x14
 * (byte offsets 0x98/0xa0); the service-mode flag is the byte at +0xc0. */
#define SVC_REGISTRY_BEGIN(self) (((void **)(self))[0x13])
#define SVC_REGISTRY_END(self)   (((void **)(self))[0x14])
#define SVC_SERVICE_FLAG(self)   (*(uint8_t *)((char *)(self) + 0xc0))

/* heartbeat trampoline installed at ctor — name "alive" / period 0x7d0 (2000ms) */
static int monitor_service_heartbeat_fn_manager(monitor_service *self,
                                                const uint8_t *frame)
{
    return monitor_service_on_heartbeat(self, frame);
}

/* ------------------------------------------------------------------------ *
 * monitor_service_build_ctx_init                                  0x1215d0  *
 * Captures the four references the component factory needs: {can, mqtt,      *
 * bus, name}. Pure field stores (param order from the decompiler).          *
 * ------------------------------------------------------------------------ */
void monitor_service_build_ctx_init(monitor_build_ctx *ctx, void *can,
                                    void *mqtt, void *name, void *bus_box)
{
    ctx->bus  = can;            /* *param_1 = param_2 (can) */
    ctx->mqtt = mqtt;           /* param_1[1] = param_3 (mqtt) */
    ctx->reset_lines = bus_box; /* param_1[2] = param_5 (bus box) */
    ctx->can  = name;           /* param_1[3] = param_4 (name) */
}

/* ------------------------------------------------------------------------ *
 * monitor_component_set_ctor                                      0x111f00  *
 * Installs the component-set vtable (PTR_FUN_00170238) and wires the POD     *
 * that the component factory consumes. *param_5 is unboxed into +0x20.      *
 * ------------------------------------------------------------------------ */
void monitor_component_set_ctor(monitor_component_set *self, void *bus,
                                void *mqtt, void *can, void **type_box,
                                void *reset_lines, void *name)
{
    void *type = *type_box;                  /* uVar1 = *param_5 */
    self->ops  = NULL;                       /* &PTR_FUN_00170238 */
    self->bus  = bus;                        /* param_1[1] */
    self->mqtt = mqtt;                       /* param_1[2] */
    self->can  = can;                        /* param_1[3] */
    self->type = type;                       /* param_1[4] */
    self->name = name;                       /* param_1[5] = param_7 */
    self->reset_lines = reset_lines;         /* param_1[6] = param_6 */
}

/* ------------------------------------------------------------------------ *
 * monitor_service_ctor                                            0x120ac0  *
 * Builds the supervisor: chains the common::Service base, zeroes the         *
 * component registry vector, stores {bus,mqtt,can,name,type,service_flag},   *
 * captures the build ctx, then registers the CANopen heartbeat callback      *
 * (manager fn "monitor_service_heartbeat_fn_manager", period 0x7d0=2000ms).  *
 * Finally it appends the supervisor's own component to the registry; the     *
 * append-with-grow path and the "runtime_error" throw are STL/EH glue.       *
 * ------------------------------------------------------------------------ */
void monitor_service_ctor(void **self, void *bus, void *can, void *mqtt,
                          void *name, void **type_box, void *set_ctx,
                          uint8_t service_flag)
{
    monitor_service *s = (monitor_service *)self;

    /* common::Service base ctor (FUN_00132430): can + mqtt + flag */
    service_base_ctor(self, can, mqtt, 0);
    s->vtbl = NULL;                          /* &DAT_00170fd0 */

    /* zero the component-registry vector + slots 0x0c..0x11 */
    memset(&self[0x0c], 0, sizeof(void *) * 6);

    self[0x12] = bus;                        /* +0x90  CAN-bus name string */
    self[0x13] = NULL;                       /* registry.begin */
    self[0x14] = NULL;                       /* registry.end   */
    self[0x15] = NULL;                       /* registry.cap   */
    self[0x16] = mqtt;                       /* +0xb0  IMQTTClient */
    self[0x17] = can;                        /* +0xb8  vm CAN context */
    SVC_SERVICE_FLAG(self) = service_flag;   /* +0xc0  service-mode flag */
    self[0x19] = *type_box;                  /* name/box */
    self[0x1a] = name;

    /* capture the build context {can, mqtt, name, bus} into +0xd8.. */
    monitor_service_build_ctx_init((monitor_build_ctx *)&self[0x1b],
                                   can, mqtt, name, &self[0x19]);

    /* register the CANopen heartbeat callback (period 0x7d0 ticks).
     * can->ops+0x18 == register_heartbeat_fn(out, can, manager_fn, &period). */
    self[0x1f] = can_register_heartbeat_fn(can, s, 0x7d0);
    (void)monitor_service_heartbeat_fn_manager; /* installed as the trampoline */

    /* append the supervisor's component to the registry (set_ctx). A duplicate
     * / capacity error throws std::runtime_error in the OEM (EH glue). */
    (void)set_ctx;
}

/* ------------------------------------------------------------------------ *
 * monitor_service_dtor                                            0x120770  *
 * Releases the heartbeat registration, tears down each registry element,     *
 * frees the vector, then the common::Service base.                           *
 * ------------------------------------------------------------------------ */
void monitor_service_dtor(void **self)
{
    void **begin, **end, **it;

    self[0] = NULL;                          /* &DAT_00170fd0 */

    /* heartbeat registration: vtbl+0x10 = deregister, then release ref (glue) */

    end   = (void **)SVC_REGISTRY_END(self);
    begin = (void **)SVC_REGISTRY_BEGIN(self);
    for (it = begin; it != end; it += 8 /* 0x40-byte elements */)
        component_registry_element_dtor(it);
    if (begin)
        op_delete(begin, 0);

    service_base_dtor(self);
}

/* ------------------------------------------------------------------------ *
 * monitor_service_supervise_components                            0x1204c0  *
 * Per-tick fan-out: drive each supervised component through its poll slot.   *
 * ------------------------------------------------------------------------ */
void monitor_service_supervise_components(monitor_service *self)
{
    icomponent **begin = (icomponent **)SVC_REGISTRY_BEGIN(self);
    icomponent **end   = (icomponent **)SVC_REGISTRY_END(self);
    icomponent **it;

    for (it = begin; it != end; ++it) {
        icomponent *comp = *it;
        comp->ops->poll(comp);               /* vtable poll slot (per-tick) */
    }
}

/* ------------------------------------------------------------------------ *
 * monitor_service_poll_tick                                       0x120530  *
 * Each tick: if we are in service mode (+0xc0 != 0), supervise components.    *
 * ------------------------------------------------------------------------ */
void monitor_service_poll_tick(monitor_service *self)
{
    if (SVC_SERVICE_FLAG(self) != 0)
        monitor_service_supervise_components(self);
}

/* ------------------------------------------------------------------------ *
 * monitor_service_run                                             0x120fe0  *
 * Spawns/joins the service worker; the base loop drives poll_tick each tick   *
 * (std::thread join + pthread_key guards are vendor). Modelled as one tick.  *
 * ------------------------------------------------------------------------ */
void monitor_service_run(monitor_service *self)
{
    monitor_service_poll_tick(self);
}

/* ------------------------------------------------------------------------ *
 * monitor_service_on_heartbeat                                    0x1208f0  *
 * CANopen heartbeat sink. Any device that wasn't registered as one of our    *
 * supervised components gets flagged. The node id is frame[2].               *
 *   monitor_service.cpp:0x3e  WARN  "Unexpected device is giving heartbeats %d."
 * ------------------------------------------------------------------------ */
int monitor_service_on_heartbeat(monitor_service *self, const uint8_t *frame)
{
    (void)self;
    common_logf("devices/main/monitor/src/monitor_service.cpp", 0x3e, LOG_INFO,
                "Unexpected device is giving heartbeats %d.", frame[2]);
    return 0;
}

/* ------------------------------------------------------------------------ *
 * monitor_set_mqtt_singleton                                      0x1492c0  *
 * Installs the process-wide MQTT client pointer + a client name (defaulting  *
 * to "ANONYMOUS"). Returns 0 on success, -2 (0xfffffffe) if slot is NULL.    *
 * ------------------------------------------------------------------------ */
int monitor_set_mqtt_singleton(void **slot, mqtt_client *mqtt, const char *name)
{
    const char *n;
    if (slot == NULL)
        return -2;                           /* 0xfffffffe */
    n = (name != NULL) ? name : "ANONYMOUS";
    slot[0] = mqtt;                          /* *param_1 = mqtt */
    slot[1] = (void *)n;                     /* param_1[1] = name */
    g_monitor_mqtt = (mqtt_client *)slot;    /* DAT_0017a970 = param_1 */
    return 0;
}

/* ------------------------------------------------------------------------ *
 * monitor_print_component_row                                     0x121ac0  *
 * Renders one component's row. This is the ground-truth decoder for the      *
 * IComponent vtable slots:                                                   *
 *   alive  = comp->ops->is_alive()   (vt+0x48)   -> ' ' or '!' marker        *
 *   name   = comp->ops->get_name()   (vt+0x30)   width 0x14, right of '|'    *
 *   status = comp->ops->get_status() (vt+0x38)   formatted cell (helper)     *
 *   value  = comp->ops->get_value()  (vt+0x50)   float seconds, "%.1fs"      *
 *   version= comp->ops->get_version()(vt+0x40)   width 0xd                   *
 *   type   = comp->ops->get_type()   (vt+0x58)   tail field                  *
 * Plus the CAN node column: when the row's node differs from the previous,    *
 * resolve can_node_id_to_ecu_name(node) (else a single space). Columns are    *
 * separated by '|'; literal cells "V:"/"BL:"/"FW:" come from the version      *
 * sub-fields. Numeric formatting is std::ostream manipulators.               *
 * ------------------------------------------------------------------------ */
void monitor_print_component_row(monitor_service *self, int global_state,
                                 uint8_t *expected_node, icomponent *comp,
                                 uint8_t *prev_node)
{
    uint8_t node;
    const char *ecu;
    char marker;
    float value;
    const char *name, *version, *type;

    (void)self;
    (void)global_state;

    node = monitor_component_node_id(expected_node);

    /* alive marker: ' ' (alive) or '!' — '!'==' '+1 when is_alive() is true */
    marker = (char)(' ' + (comp->ops->is_alive(comp) != false));
    putchar(marker);
    putchar(' ');

    /* CAN node column: ECU name unless unchanged from the previous row */
    if (*prev_node == node) {
        fputs(" ", stdout);
    } else {
        ecu = can_node_id_to_ecu_name(node);
        if (ecu != NULL)
            fputs(ecu, stdout);              /* else unresolved: leave blank */
    }
    putchar('|');

    /* NAME column (width 0x14) */
    name = comp->ops->get_name(comp);
    fputs(name ? name : "", stdout);
    putchar('|');

    /* STATUS cell — helper formats the raw status code */
    monitor_print_status_cell(*expected_node);
    putchar('|');

    /* VALUE column: get_value() seconds, printed "%.1f" then 's' */
    value = comp->ops->get_value(comp);
    printf("%.1f", (double)value);
    putchar('s');
    putchar('|');

    /* VERSION sub-fields: "V:" "BL:" "FW:" literal pad cells (DAT_0014e810/.../e830) */
    fputs("V:", stdout);
    putchar(' ');
    fputs("BL:", stdout);
    putchar(' ');
    fputs("FW:", stdout);
    putchar('|');

    /* VERSION column (width 0xd): get_version() */
    version = comp->ops->get_version(comp);
    fputs(version ? version : "", stdout);
    putchar('|');

    /* TYPE tail: get_type() */
    type = comp->ops->get_type(comp);
    fputs(type ? type : "", stdout);

    putchar('\n');
}

/* ------------------------------------------------------------------------ *
 * monitor_print_component_status_table                            0x1221f0  *
 * Repaints the whole-screen status table:                                    *
 *   "\x1b[2J\x1b[1;1H\n"        clear screen + cursor home                    *
 *   "GLOBAL STATE: " <state>   | " SYSTEM HEALTH: HEALTHY"                    *
 *   a 0x6d-dash rule, then one row per supervised component.                 *
 * `global_state` is decoded for display by monitor_decode_global_state.      *
 * ------------------------------------------------------------------------ */
void monitor_print_component_status_table(monitor_service *self, int global_state)
{
    icomponent **begin = (icomponent **)SVC_REGISTRY_BEGIN(self);
    icomponent **end   = (icomponent **)SVC_REGISTRY_END(self);
    icomponent **it;
    uint8_t prev_node = 0;            /* local_32 */

    fputs("\x1b[2J\x1b[1;1H\n", stdout);          /* DAT_0014e838 */
    fputs("GLOBAL STATE: ", stdout);
    (void)monitor_decode_global_state(global_state);
    fputs("               | SYSTEM HEALTH: HEALTHY\n", stdout);
    fputs("---------------------------------------------------------------"
          "----------------------------------------------\n", stdout); /* 0x6d dashes */

    for (it = begin; it != end; ++it) {
        icomponent *comp = *it;
        uint8_t node;

        /* poll the component (vtbl+0x28) then render its row */
        comp->ops->poll(comp);
        node = monitor_component_node_id(&prev_node);
        monitor_print_component_row(self, global_state, &node, comp, &prev_node);
        prev_node = node;
    }
}
