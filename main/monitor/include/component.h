/*
 * component.h — module-private declarations for the reconstructed VanMoof S5
 * i.MX8 `monitor` IComponent base + GPIO reset bank (component.cpp,
 * common/gpio.cpp). Included AFTER monitor_common.h.
 *
 * Program "monitor", AArch64, image base 0x100000. Only the VanMoof-authored
 * application logic is reconstructed here; the C++ runtime / std::string /
 * std::ofstream / mosquitto / std::thread glue is VENDOR and is modelled as
 * opaque calls.
 */
#ifndef MONITOR_COMPONENT_H
#define MONITOR_COMPONENT_H

#include "monitor_common.h"

/* ----- component_format_version_string (0x1371e0) ----------------------- *
 * Builds the "<major>.<minor>.<patch>" version string. In the OEM image the
 * three integers are baked in (1, 5, 0) -> "1.5.0"; the routine snprintf("%d")
 * each field and joins with '.'. Modelled: writes into the caller buffer.    */
char *component_format_version_string(char *out /* std::string* */);

/* ----- component_mqtt_client_ctor (0x13a8d0) ---------------------------- *
 * The "MQTTClient" IComponent: chains icomponent_base_ctor, installs its own
 * vtable + the mosquitto-backed pub/sub ring buffers, connects to the broker
 * host:port keepalive, then spins up the network thread. On a non-zero connect
 * result it throws std::runtime_error("Mosquitto connect error: <rc>"). The
 * ring-buffer sizing, std::thread, condition-variable wait and exception glue
 * are vendor; modelled here as the connect + thread-start behaviour.          */
void component_mqtt_client_ctor(icomponent *self, const char *host,
                                const char *host_alt, uint32_t keepalive,
                                const char *client_id, void *user_ctx);

/* ----- component_registry_element_dtor (0x147b20) ----------------------- *
 * Destructor for one entry of the supervisor's component registry: frees the
 * embedded std::string name (if heap-allocated, i.e. data ptr != SSO buffer at
 * +0x30) then calls the held IComponent's deleting dtor (vtable slot 0).      */
void component_registry_element_dtor(void *element);

/* ----- GPIO reset line ctor (0x134bf0) ---------------------------------- *
 * Wraps gpio_export_and_configure(pin, out=false) then drives the initial
 * level per `mode`: mode 0 -> set active level; mode 1 -> set inverted; other
 * -> leave. `active` is the active-high(1)/low(0) polarity stored at +0x10.   */
void component_gpio_reset_line_ctor(gpio_reset_line *self, int pin, int mode,
                                    int active);

/* ----- reset-line bank ctor (0x120310) ---------------------------------- *
 * Constructs the four reset lines on GPIO pins {1, 0, 10, 11}, each mode 2
 * (leave level untouched) active-high(0).                                     */
typedef struct component_reset_lines {
    const void      *vtable;     /* +0x00 (&DAT_00170340) */
    gpio_reset_line  line[4];    /* pins 1, 0, 10, 11 */
} component_reset_lines;
void component_reset_lines_ctor(component_reset_lines *self);

#endif /* MONITOR_COMPONENT_H */
