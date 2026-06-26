/*
 * monitor_service.h — module-local declarations for the reconstructed VanMoof
 * S5 i.MX8 `monitor` MonitorService (component supervisor) and its CLI/bring-up
 * (main.cpp + monitor_service.cpp + service_env.cpp). Included AFTER
 * monitor_common.h.  Program "monitor", AArch64, image base 0x100000.
 *
 * The framework objects below (component registry vector, CANopen heartbeat-
 * function manager, std::thread runner, the C++ service_env, getopt) are VENDOR
 * glue: modelled as opaque handles + extern prototypes so the behaviour reads
 * clean in C. OEM addresses are quoted at each function.
 */
#ifndef MONITOR_SERVICE_H
#define MONITOR_SERVICE_H

#include "monitor_common.h"

/* ---- opaque vendor framework handles ----------------------------------- */
typedef struct vm_can         vm_can;          /* libvm SocketCAN/CANopen ctx */
typedef struct hb_fn_handle   hb_fn_handle;    /* CANopen heartbeat callback reg */
typedef struct comp_registry  comp_registry;   /* std::vector<IComponent*>     */
typedef struct reset_lines    reset_lines;     /* component_reset_lines (4 GPIO)*/

/* component build context: {bus, mqtt, reset_lines, can} captured for the
 * component factory (monitor_service_build_ctx_init @0x1215d0). */
typedef struct monitor_build_ctx {
    void *bus;          /* +0x00 CAN-bus name string */
    void *mqtt;         /* +0x08 IMQTTClient */
    void *reset_lines;  /* +0x10 reset-line bank */
    void *can;          /* +0x18 vm CAN context */
} monitor_build_ctx;

/* The component-set descriptor (monitor_component_set_ctor @0x111f00):
 * a small POD wiring {bus, mqtt, can, type, reset_lines, name} behind
 * vtable PTR_FUN_00170238; consumed by the component factory. */
typedef struct monitor_component_set {
    const void *ops;    /* +0x00 &PTR_FUN_00170238 */
    void *bus;          /* +0x08 */
    void *mqtt;         /* +0x10 */
    void *can;          /* +0x18 */
    void *type;         /* +0x20 (from *param_5) */
    void *name;         /* +0x28 (param_7) */
    void *reset_lines;  /* +0x30 (param_6) */
} monitor_component_set;

/* MonitorService instance (allocated 0x100 bytes @ main). The OEM object is
 * accessed in the .c by raw word index (self[0x12]=bus, 0x16=mqtt, 0x17=can,
 * 0x18(byte)=service-flag at +0xc0, 0x13/0x14=registry begin/end, 0x1f=hb reg);
 * the named fields below cover only what the C model references by name. */
typedef struct monitor_service {
    const void *vtbl;                       /* +0x00  &DAT_00170fd0 */
    char        opaque[0x100 - sizeof(void *)]; /* rest of the 0x100-byte object */
} monitor_service;

/* ---- service ctor/dtor/run/poll ---------------------------------------- */
void  monitor_service_ctor(void **self, void *bus, void *can, void *mqtt,
                           void *name, void **type_box, void *set_ctx,
                           uint8_t service_flag);                 /* 0x120ac0 */
void  monitor_service_dtor(void **self);                          /* 0x120770 */
void  monitor_service_run(monitor_service *self);                 /* 0x120fe0 */
void  monitor_service_poll_tick(monitor_service *self);           /* 0x120530 */
void  monitor_service_supervise_components(monitor_service *self);/* 0x1204c0 */
int   monitor_service_on_heartbeat(monitor_service *self, const uint8_t *frame); /* 0x1208f0 */
void  monitor_service_build_ctx_init(monitor_build_ctx *ctx, void *can,
                                     void *mqtt, void *name, void *bus_box); /* 0x1215d0 */
void  monitor_component_set_ctor(monitor_component_set *self, void *bus,
                                 void *mqtt, void *can, void **type_box,
                                 void *reset_lines, void *name);   /* 0x111f00 */
int   monitor_set_mqtt_singleton(void **slot, mqtt_client *mqtt, const char *name); /* 0x1492c0 */

/* ---- status-table renderer (service mode) ------------------------------ */
void  monitor_print_component_status_table(monitor_service *self, int global_state); /* 0x1221f0 */
void  monitor_print_component_row(monitor_service *self, int global_state,
                                  uint8_t *expected_node, icomponent *comp,
                                  uint8_t *prev_node);             /* 0x121ac0 */

/* vendor glue invoked by the above (modelled) */
extern mqtt_client *g_monitor_mqtt;            /* DAT_0017a970 singleton slot */
void  service_base_ctor(void **self, void *can, void *mqtt, int flag);    /* FUN_00132430 */
void  service_base_dtor(void *self);                                      /* FUN_001321a0 */
hb_fn_handle *can_register_heartbeat_fn(void *can, void *self, int node); /* via can+0x18 */
void  component_registry_element_dtor(void *elem);
int   monitor_decode_global_state(int raw);                              /* FUN_00131940 */
uint8_t monitor_component_node_id(uint8_t *expected);                    /* FUN_0012ce70 */
void  monitor_print_status_cell(uint8_t raw);                            /* FUN_001215e0 */

#endif /* MONITOR_SERVICE_H */
