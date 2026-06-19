#include "update_common.h"

/* ============ module-local framework model (externs + structs) ============ */
typedef struct update_client update_client;
typedef struct smp_modem_target smp_modem_target;
typedef struct od_registry od_registry;
typedef struct update_request update_request;
/* Construction context (the IUpdateClient request descriptor) passed in x1.
 * Offsets match the OEM field loads in UpdateClientFactory_GetUpdateClient. */
typedef struct factory_ctx {
    uint8_t           _pad0[8];
    serial_transport *transport_a;     /* +0x08 */
    serial_transport *transport_b;     /* +0x10 */
    void             *smp_bus;         /* +0x18 : MQTT bus for SMP/modem path */
    serial_transport *motor_transport; /* +0x20 : motor_control transport */
    od_registry      *od_registry;     /* +0x28 : OD/registry getInt object */
    uint8_t           client_flag;     /* +0x30 : one-byte flag forwarded to ctors */
} factory_ctx;

/* Lightweight C2000 client retry/timeout triple (OEM stack {0x3a98,0x7d0,0xaba9500}). */
typedef struct lw_timeouts {
    uint64_t retry_interval_ms;  /* 15000     */
    uint64_t op_timeout_ms;      /* 2000      */
    uint64_t total_timeout_us;   /* 180000000 */
} lw_timeouts;

/* Opaque framework / vtable types modelled, not rebuilt. */
typedef struct update_client update_client;
typedef struct smp_modem_target smp_modem_target;
typedef struct od_registry od_registry;
typedef struct serial_transport serial_transport;
typedef struct update_request update_request;
/* libc */
int strcasecmp(const char *a, const char *b);
/* .bss std::string token globals, built in _INIT_6 (OEM 0x10d854).
 * Modelled as C strings; OEM addresses given for traceability. */
extern const char *g_token_motor_control;       /* OEM .bss 0x1a2148: "motor_control" */
extern const char *g_token_battery_panasonic;   /* OEM .bss 0x1a2168: "battery_primary_panasonic" */
extern const char *g_token_battery_dynapack;    /* OEM .bss 0x1a21a8: "battery_primary_dynapack"  */
extern const char *g_token_charger;             /* OEM .bss 0x1a21c8: "charger" */
/* OD registry key for the battery firmware version (od->getInt at ctx+0x28). */
#define OD_KEY_BATTERY_VERSION "version"
bool od_get_int(od_registry *od, const char *key, int *out);
/* Extract the request name into a caller buffer (OEM FUN_00123ae0:
 * a std::string copy of the field at req+0x28/0x30). */
void req_get_name(char *dst, size_t cap, const update_request *req);
/* Concrete client constructors (each operator new + ctor in the OEM). */
update_client *make_motor_control_client(serial_transport *transport,
                                         const update_request *req,
                                         uint8_t flag);                 /* OEM 0x130300 */
update_client *PanasonicUpdateClient_ctor(serial_transport *ta, serial_transport *tb,
                                          uint8_t node, const update_request *req,
                                          uint8_t flag, bool fast_mode); /* OEM 0x13b6a0 */
update_client *DynapackUpdateClient_ctor(serial_transport *ta, serial_transport *tb,
                                         uint8_t node, const update_request *req,
                                         uint8_t flag);                  /* OEM 0x13b620 */
update_client *LiteonUpdateClient_ctor(serial_transport *ta, serial_transport *tb,
                                       uint8_t node, const update_request *req,
                                       uint8_t flag);                    /* OEM 0x13b750 */
update_client *lightweight_update_client_ctor(serial_transport *ta, serial_transport *tb,
                                              const lw_timeouts *to,
                                              const update_request *req,
                                              uint8_t flag);             /* OEM 0x133370 */
/* SMP / modem FTP target + client construction. */
smp_modem_target *make_smp_modem_target(void *mqtt_bus, const char *req_name); /* OEM 0x13f540 */
update_client *make_smp_modem_client(smp_modem_target **owned_target,
                                     const update_request *req);              /* OEM 0x136c00 */
void smp_modem_target_release(smp_modem_target *t);
/* device-name -> code parser (this TU defines it; declared for callers). */
uint8_t device_code_from_name(const char *name, uint8_t *recognised);    /* OEM 0x12fbc0 */
/* std::invalid_argument throwers (the two diagnostic paths). Never return. */
_Noreturn void throw_invalid_argument_unprocessable(const update_request *req);
_Noreturn void throw_invalid_argument_bad_supplier(const update_request *req);
/* ========================================================================== */

/*
 * UpdateClientFactory — supplier/device dispatch for the OTA "update" service.
 *
 * Reconstructed from the AArch64 "update" image (base 0x100000).
 *
 * The factory receives a request descriptor (the IUpdateClient construction
 * context). From it we read:
 *   ctx + 0x08 / 0x10 : the two transport / VM-call handles
 *   ctx + 0x18        : the modem/SMP MQTT bus handle (used by the SMP path)
 *   ctx + 0x20        : the motor_control transport handle
 *   ctx + 0x28        : the OD/registry object (->getInt("...") style accessor)
 *   ctx + 0x30        : a one-byte "verbose/flag" passed to every client ctor
 * and the request name string lives at ctx + 0x28 / 0x30 of the *name* object
 * (extracted by req_get_name() == OEM FUN_00123ae0).
 *
 * Dispatch order (matches the OEM, first match wins):
 *   1. name contains "motor_control"             -> motor_control client
 *   2. name contains "battery_primary_panasonic" -> Panasonic client (node 0xA4),
 *                                                   version-gated fast/slow mode
 *   3. name contains "battery_primary_dynapack"  -> Dynapack client  (node 0xA4)
 *   4. name contains "charger"                    -> Liteon client    (node 0xA7)
 *   5. else: parse the leading device token -> device code byte
 *        - code 0xA4  : battery_primary with an unrecognised supplier -> throw
 *        - code 0x90/0x91 (BLE / MODEM) : SMP / modem FTP client
 *        - any other recognised code     : lightweight C2000 client (node 0xA4)
 *        - unrecognised token            : throw "cannot be processed"
 *
 * The .bss token globals (constructed in _INIT_6) are:
 *   "motor_control", "battery_primary", "_panasonic", "_dynapack", "charger".
 */

/* Battery node id passed to the battery / lightweight clients (OD 0xA4). */
#define NODE_BATTERY_PRIMARY   ((uint8_t)0xA4)
/* Charger node id passed to the Liteon client (OD 0xA7). */
#define NODE_CHARGER           ((uint8_t)0xA7)

/* Panasonic version gate: build the "fast" client iff version > 0x010300FF. */
#define PANASONIC_VERSION_GATE 0x010300FFu

/* Device codes routed to the SMP / modem FTP client (FUN_0012fbc0 results). */
#define DEVCODE_BLE            ((uint8_t)0x90)
#define DEVCODE_MODEM          ((uint8_t)0x91)
/* Device code that means "battery_primary" with no recognised supplier suffix. */
#define DEVCODE_BATTERY_PRIMARY ((uint8_t)0xA4)

/* Lightweight C2000 client retry/timeout triple (verbatim OEM constants). */
#define LW_RETRY_INTERVAL_MS   15000      /* 0x3a98     */
#define LW_OP_TIMEOUT_MS       2000       /* 0x7d0      */
#define LW_TOTAL_TIMEOUT_US    180000000  /* 0xaba9500  */

/*
 * UpdateClientFactory_GetUpdateClient   (OEM 0x123220)
 *
 * Selects and constructs the concrete IUpdateClient for a request.
 * Returns the new client; throws std::invalid_argument when the request name
 * cannot be matched to any supported device/supplier.
 */
update_client *UpdateClientFactory_GetUpdateClient(const factory_ctx *ctx,
                                                   const update_request *req)
{
    char name[256];

    /* --- 1. motor_control -------------------------------------------------- */
    req_get_name(name, sizeof(name), req);              /* FUN_00123ae0 */
    if (str_contains(name, g_token_motor_control)) {    /* .bss "motor_control" */
        /* operator new(0x20); ctor FUN_00130300 */
        return make_motor_control_client(ctx->motor_transport,  /* ctx+0x20 */
                                         req,
                                         ctx->client_flag);     /* ctx+0x30 */
    }

    /* --- 2. battery_primary_panasonic -------------------------------------- */
    req_get_name(name, sizeof(name), req);
    if (str_contains(name, g_token_battery_panasonic)) { /* "battery_primary_panasonic" */
        int version;
        bool fast_mode = false;

        /* od->getInt("...") at ctx+0x28 ; returns {valid, value}. */
        if (od_get_int(ctx->od_registry, OD_KEY_BATTERY_VERSION, &version)) {
            fast_mode = ((uint32_t)version > PANASONIC_VERSION_GATE);
        }

        /* operator new(0x140); ctor PanasonicUpdateClient_ctor (0x13b6a0) */
        return PanasonicUpdateClient_ctor(
                   ctx->transport_a, ctx->transport_b,   /* ctx+0x08, +0x10 */
                   NODE_BATTERY_PRIMARY,
                   req,
                   ctx->client_flag,                     /* ctx+0x30 */
                   fast_mode);
    }

    /* --- 3. battery_primary_dynapack --------------------------------------- */
    req_get_name(name, sizeof(name), req);
    if (str_contains(name, g_token_battery_dynapack)) {  /* "battery_primary_dynapack" */
        /* operator new(0x140); ctor DynapackUpdateClient_ctor (0x13b620) */
        return DynapackUpdateClient_ctor(
                   ctx->transport_a, ctx->transport_b,
                   NODE_BATTERY_PRIMARY,
                   req,
                   ctx->client_flag);
    }

    /* --- 4. charger -------------------------------------------------------- */
    req_get_name(name, sizeof(name), req);
    if (str_contains(name, g_token_charger)) {           /* "charger" */
        /* operator new(0x1a0); ctor LiteonUpdateClient_ctor (0x13b750) */
        return LiteonUpdateClient_ctor(
                   ctx->transport_a, ctx->transport_b,
                   NODE_CHARGER,
                   req,
                   ctx->client_flag);
    }

    /* --- 5. fall through: parse the leading device token ------------------- */
    req_get_name(name, sizeof(name), req);
    {
        uint8_t recognised = 0;                          /* local_e1 */
        /* device_code_from_name() == FUN_0012fbc0: returns code byte and sets
         * 'recognised' to 0 only for the final unmatched "TEST"/default case. */
        uint8_t code = device_code_from_name(name, &recognised);

        if (!recognised) {
            /* "Error, update file '<name>' for device '<name>', cannot be
             * processed" */
            throw_invalid_argument_unprocessable(req);   /* never returns */
        }

        if (code == DEVCODE_BATTERY_PRIMARY) {
            /* battery_primary token but the supplier suffix did not match any
             * of the cases above. */
            throw_invalid_argument_bad_supplier(req);    /* never returns */
        }

        /* (uint8_t)(code + 0x70) < 2  <=>  code in { 0x90, 0x91 }. */
        if ((uint8_t)(code + 0x70u) < 2u) {
            /* BLE (0x90) / MODEM (0x91): SMP / modem FTP client. */
            smp_modem_target *tgt;

            req_get_name(name, sizeof(name), req);
            /* operator new(0xe0); ctor FUN_0013f540: picks the MQTT topic set
             * (ble/ftp/firmware, modem/ftp, ftp_server, phone/ftp_server) from
             * the request name, throws std::runtime_error("invalid target") if
             * none match. */
            tgt = make_smp_modem_target(ctx->smp_bus, name); /* ctx+0x18 */

            /* operator new(0x18); ctor FUN_00136c00 wraps the target into the
             * IUpdateClient (takes ownership; the temporary is released). */
            {
                update_client *cli = make_smp_modem_client(&tgt, req);
                if (tgt != NULL) {
                    smp_modem_target_release(tgt);
                }
                return cli;
            }
        }

        /* Any other recognised device code: lightweight C2000 client. */
        {
            lw_timeouts to;
            to.retry_interval_ms = LW_RETRY_INTERVAL_MS;
            to.op_timeout_ms     = LW_OP_TIMEOUT_MS;
            to.total_timeout_us  = LW_TOTAL_TIMEOUT_US;

            /* operator new(0x130); ctor lightweight_update_client_ctor
             * (0x133370): node 0xA4, VM ops flash / ErasePage / write / reboot. */
            return lightweight_update_client_ctor(
                       ctx->transport_a, ctx->transport_b,
                       &to,
                       req,
                       ctx->client_flag);
        }
    }
}

/*
 * device_code_from_name   (OEM 0x12fbc0)
 *
 * Case-insensitive map from a device-name token to its OD/CAN address byte.
 * *recognised (param_2) is set to 1 on entry and cleared to 0 only when the
 * token matches none of the names (the trailing "TEST"/default branch). The
 * "TEST" token itself still yields 0xFF but leaves *recognised == 1.
 *
 * Table reproduced verbatim from the OEM strcasecmp chain.
 */
uint8_t device_code_from_name(const char *name, uint8_t *recognised)
{
    static const struct {
        const char *token;   /* primary spelling */
        const char *alt;     /* alternate spelling (dash form), or NULL */
        uint8_t     code;
    } table[] = {
        { "CORE_SERVICES",     "CORE-SERVICES",     0x00 },
        { "HEARTBEAT",         NULL,                0x08 },
        { "POWER",             NULL,                0x82 },
        { "RIDE",              NULL,                0x84 },
        { "LOGGING",           NULL,                0x87 },
        { "UX",                NULL,                0x88 },
        { "UPDATE",            NULL,                0x8A },
        { "MQTT_OD_BRIDGE",    "MQTT-OD-BRIDGE",    0x8B },
        { "MQTT_FTP",          "MQTT-FTP",          0x8C },
        { "MOTOR_CONTROL",     "MOTOR-CONTROL",     0x8D },
        { "IMX8_BRIDGE",       "IMX8-BRIDGE",       0x8F },
        { "BLE",               NULL,                0x90 },
        { "MODEM",             NULL,                0x91 },
        { "MOTOR_SENSOR",      "MOTOR-SENSOR",      0xA1 },
        { "POWER_PEDAL",       "POWER-PEDAL",       0xA2 },
        { "POWER_CONTROL",     "POWER-CONTROL",     0xA3 },
        { "BATTERY_PRIMARY",   "BATTERY-PRIMARY",   0xA4 },
        { "BATTERY_SECONDARY", "BATTERY-SECONDARY", 0xA5 },
        { "CHARGER",           NULL,                0xA7 },
        { "USER_ECU",          "USER-ECU",          0xC0 },
        { "ELOCK",             NULL,                0xC1 },
        { "ESHIFTER",          NULL,                0xC2 },
        { "REARLIGHT",         NULL,                0xC3 },
        { "FRONTLIGHT",        NULL,                0xC4 },
        { "PHONE",             NULL,                0xE0 },
        { "BACKOFFICE",        NULL,                0xE2 },
        { "PING",              NULL,                0xFB },
        { "DUMMY",             NULL,                0xFC },
        { "BATTERY_TEST",      "BATTERY-TEST",      0xFD },
        { "RASPBERRY",         NULL,                0xFE },
        /* "TEST" handled below: code 0xFF, still recognised. */
    };
    size_t i;

    if (recognised != NULL) {
        *recognised = 1;
    }

    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(name, table[i].token) == 0 ||
            (table[i].alt != NULL && strcasecmp(name, table[i].alt) == 0)) {
            return table[i].code;
        }
    }

    /* Default branch: 0xFF; clear *recognised unless the token is exactly
     * "TEST" (which keeps the recognised flag set). */
    if (recognised != NULL && strcasecmp(name, "TEST") != 0) {
        *recognised = 0;
    }
    return 0xFF;
}
