/*
 * lightweight_update.h — model for the VanMoof S5 i.MX8 `lightweight_update`
 * CLI flasher (`/usr/bin/lightweight_update`, pkg vmxs5-embedded-lightweight-update).
 *
 * This binary is a TRIMMED, standalone command-line front-end to the same update
 * machinery the `update` service uses. Its only VanMoof-authored source is
 * `utils/lightweight_update/main.cpp` (reconstructed in src/main.c); every update
 * client and the vm/MQTT/version plumbing is the **shared `update` code** already
 * reconstructed under ../update/ — modelled here as opaque externs (not
 * re-reconstructed). Program "lightweight_update", AArch64, image base 0x100000.
 */
#ifndef LIGHTWEIGHT_UPDATE_H
#define LIGHTWEIGHT_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

/* ---- shared `update` module: opaque handles (see ../update/) ------------ */
typedef struct vm_ctx                 vm_ctx;                 /* libvm SocketCAN/CANopen ctx (0x2860 bytes) */
typedef struct mqtt_client            mqtt_client;           /* common::IMQTTClient (mosquitto) */
typedef struct version_client         version_client;        /* VersionClientMqtt (version_client_mqtt.cpp) */
typedef struct update_client_factory  update_client_factory; /* UpdateClientFactory */
typedef struct iupdate_client         iupdate_client;        /* IUpdateClient (Lightweight/C2000/Mqtt/ThirdParty) */

/* vm context: allocate 0x2860, zero, vm_init (FUN_00133560). */
vm_ctx *vm_ctx_alloc_init(void);                                  /* 0x108430+0x133560 */
/* open the SocketCAN channel (FUN_00133fb0 @ vm+0x2828); 0 == success. */
int     vm_ctx_open_can(vm_ctx *vm, const char *can_bus);         /* 0x133fb0 */
void    vm_ctx_free(vm_ctx *vm);                                  /* 0x108440 (free 0x2860) */

/* common::IMQTTClient(client_id, user, clean_session, host, port). NB the OEM
 * ctor (0x134db0) hardcodes the mosquitto connect to localhost:1883 keepalive
 * 60; its 6th literal arg (5000) is a client field at +0x58, not the port. */
mqtt_client    *mqtt_client_new(const char *client_id, const char *user,
                                int clean, const char *host, int port); /* 0x134db0 */
void            mqtt_client_delete(mqtt_client *c);                     /* 0x135390 */

/* VersionClientMqtt(mqtt) — subscribes device/+/version/{firmware,bootloader,vendor}/#. */
version_client *version_client_mqtt_new(void);                          /* 0x142890 */
void            version_client_delete(version_client *v);

/* UpdateClientFactory(vm, version_client, …, force) and its lookup. */
update_client_factory *update_client_factory_new(vm_ctx *vm, version_client *vc,
                                                 mqtt_client *mqtt, bool force); /* 0x10b300 */
void            update_client_factory_delete(update_client_factory *f);
/* GetUpdateClient(file_path, device_name, force) -> IUpdateClient* or NULL
 * (factory vtable +0x10). */
iupdate_client *factory_get_update_client(update_client_factory *f,
                                          const char *file_path,
                                          const char *device_name,
                                          bool force);                  /* (*f+0x10) */

/* IUpdateClient::PerformUpdate() (client vtable +0x10): returns 0 on success,
 * non-zero if the update was NOT performed. */
int             iupdate_client_perform_update(iupdate_client *c);       /* (*c+0x10) */
void            iupdate_client_delete(iupdate_client *c);               /* (*c+8) */

/* Map a numeric target address / enum to its device-name string; returns the
 * sentinel "invalid enum value" when out of range (FUN_0010f230). */
const char     *target_addr_to_device_name(uint8_t addr);              /* 0x10f230 */

/* monotonic clock in nanoseconds (FUN_00107dc0); elapsed/1e6 == milliseconds. */
long            clock_ns(void);                                        /* 0x107dc0 */

/* global verbose flag toggled by -v (PTR_DAT_0016ef40). */
extern int      g_lwu_verbose;

#endif /* LIGHTWEIGHT_UPDATE_H */
