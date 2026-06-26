/*
 * lpc_modem.h — module-private model for the monitor LPC MotorController and
 * Nordic modem IComponent subclasses. Included AFTER monitor_common.h.
 *
 * Both components are modelled (bodies inlined / RELA / in an undisassembled
 * dispatch gap); these structs hold the per-instance state the overrides read.
 */
#ifndef MONITOR_LPC_MODEM_H
#define MONITOR_LPC_MODEM_H

#include "monitor_common.h"

/* ---- LPC MotorController (CAN node 0xA1 = "Motor") ---------------------- */
enum lpc_supplier {
    LPC_SUPPLIER_PANASONIC = 0,   /* _panasonic     */
    LPC_SUPPLIER_DYNAPACK,        /* _dynapack      */
    LPC_SUPPLIER_LITEON_NORMAL,   /* _liteon_normal */
    LPC_SUPPLIER_LITEON_SPEED,    /* _liteon_speed  */
};

typedef struct motor_controller {
    icomponent        base;               /* +0x00 IComponent (ops + mqtt) */
    enum lpc_supplier supplier;
    bool              alive;
    float             seconds_since_seen;
    char              version[32];        /* firmware version + supplier suffix */
    char              bootloader_version[32];
    char              vendor[32];
} motor_controller;

const char *lpc_supplier_suffix(enum lpc_supplier s);
void motor_controller_ctor(motor_controller *mc, mqtt_client *mqtt,
                           enum lpc_supplier supplier);

/* ---- Nordic nRF9160 modem ---------------------------------------------- */
typedef struct modem_component {
    icomponent base;                      /* +0x00 IComponent (ops + mqtt) */
    bool       alive;
    float      seconds_since_alive;
    char       charger_type[32];          /* published to monitor/component/charger/type */
} modem_component;

void modem_ctor(modem_component *m, mqtt_client *mqtt);

#endif /* MONITOR_LPC_MODEM_H */
