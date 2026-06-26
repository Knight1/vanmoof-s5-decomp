/*
 * modem.c — VanMoof S5 i.MX8 `monitor` service: the cellular modem IComponent
 * (C++ class `modem`). Supervises the Nordic nRF9160 modem module: aliveness
 * is determined by ICMP ping to public anycast resolvers, and identity is the
 * modem firmware build string.
 *
 * Program "monitor", AArch64, image base 0x100000.
 *
 * MODELLED — honest reconstruction. The modem IComponent body lives in an
 * undisassembled indirect-dispatch gap (no defined OEM function; the ctor and
 * the ping/announce logic are inlined / off the disassembly), so no
 * instruction-level OEM addresses are quoted here. What IS documented and is
 * reproduced verbatim:
 *   - aliveness = ICMP ping to 1.1.1.1 and 8.8.4.4 ("ping <ip>"); log strings
 *     "Ping successful" / "Ping failed" / "Error executing ping command" at
 *     modem.cpp.
 *   - reported version mfw_nrf9160_1.3.1; component type modem_nordic_stack.
 *   - the charger-type is published to monitor/component/charger/type.
 */
#include "monitor_common.h"
#include "lpc_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEM_VERSION   "mfw_nrf9160_1.3.1"
#define MODEM_TYPE      "modem_nordic_stack"

/* The two anycast targets pinged for aliveness (Cloudflare + Google). */
static const char *const k_ping_targets[] = { "1.1.1.1", "8.8.4.4" };

/*
 * modem_ping_target — run a single ICMP ping ("ping <ip>") via the shell and
 * classify the result. MODELLED on the documented modem.cpp log strings.
 * Returns true on a successful reply.
 */
static bool modem_ping_target(const char *ip)
{
    char cmd[64];
    int  rc;

    (void)snprintf(cmd, sizeof cmd, "ping -c 1 -W 1 %s", ip);
    rc = system(cmd);

    if (rc == -1) {
        common_logf("modem.cpp", 0, LOG_ERROR, "Error executing ping command");
        return false;
    }
    if (rc == 0) {
        common_logf("modem.cpp", 0, LOG_INFO, "Ping successful");
        return true;
    }
    common_logf("modem.cpp", 0, LOG_WARN, "Ping failed");
    return false;
}

/* ---- IComponent overrides (MODELLED) ----------------------------------- */

/* vt+0x30 get_name() */
static const char *modem_get_name(icomponent *self)
{
    (void)self;
    return "modem";
}

/* vt+0x38 get_status() */
static const char *modem_get_status(icomponent *self)
{
    modem_component *m = (modem_component *)self;
    return m->alive ? "ok" : "error";
}

/* vt+0x40 get_version() — Nordic modem firmware build. */
static const char *modem_get_version(icomponent *self)
{
    (void)self;
    return MODEM_VERSION;
}

/* vt+0x48 is_alive() — true if either anycast target answered. */
static bool modem_is_alive(icomponent *self)
{
    modem_component *m = (modem_component *)self;
    return m->alive;
}

/* vt+0x50 get_value() — seconds since the last successful ping. */
static float modem_get_value(icomponent *self)
{
    modem_component *m = (modem_component *)self;
    return m->seconds_since_alive;
}

/* vt+0x58 get_type() */
static const char *modem_get_type(icomponent *self)
{
    (void)self;
    return MODEM_TYPE;
}

/*
 * modem_publish_charger_type — re-publishes the detected charger supplier tag
 * to monitor/component/charger/type (documented topic). MODELLED.
 */
static void modem_publish_charger_type(modem_component *m, const char *type)
{
    mqtt_publish_str(m->base.mqtt, "monitor/component/charger/type", type, 1, 0);
}

/* vt poll() — per-tick supervise: ping both targets; alive if either answers. */
static void modem_poll(icomponent *self)
{
    modem_component *m = (modem_component *)self;
    bool   alive = false;
    size_t i;

    for (i = 0; i < sizeof k_ping_targets / sizeof k_ping_targets[0]; i++) {
        if (modem_ping_target(k_ping_targets[i])) {
            alive = true;
            break;
        }
    }
    m->alive = alive;
    if (alive)
        m->seconds_since_alive = 0.0f;

    if (m->charger_type[0] != '\0')
        modem_publish_charger_type(m, m->charger_type);
}

static void modem_dtor(icomponent *self)
{
    icomponent_base_dtor(self);
}

static const icomponent_ops k_modem_ops = {
    .dtor        = modem_dtor,
    .get_name    = modem_get_name,
    .get_status  = modem_get_status,
    .get_version = modem_get_version,
    .is_alive    = modem_is_alive,
    .get_value   = modem_get_value,
    .get_type    = modem_get_type,
    .poll        = modem_poll,
};

/* modem_ctor — MODELLED constructor (base vtable + mosquitto, then override). */
void modem_ctor(modem_component *m, mqtt_client *mqtt)
{
    icomponent_base_ctor(&m->base, mqtt);
    m->base.ops           = &k_modem_ops;
    m->alive              = false;
    m->seconds_since_alive = 0.0f;
    m->charger_type[0]    = '\0';
}
