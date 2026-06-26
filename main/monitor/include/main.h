/*
 * main.h — CLI / bring-up declarations for the reconstructed VanMoof S5 i.MX8
 * `monitor` entry point (main.cpp + service_env.cpp). Included AFTER
 * monitor_common.h and monitor_service.h.  Program "monitor", base 0x100000.
 */
#ifndef MONITOR_MAIN_H
#define MONITOR_MAIN_H

#include "monitor_common.h"

/* ---- parsed CLI options (main_parse_args output) ----------------------- */
typedef struct monitor_args {
    char  *bus;          /* -b <iface> : SocketCAN interface name */
    bool   service;      /* -s/--service : briefer output + user alerts */
} monitor_args;

/* getopt long-option set added by main_parse_args (FUN_001304d0):
 *   --verbose  --version  (NULL term) over the short opts "vVb:h". */
long *main_parse_args(long *opts_out, int argc, char **argv,
                      char **bus, void *long_opts, void *getopt_state,
                      long help_throw);                       /* 0x1304d0 */

/* service_env: vm_init + IMQTTClient(localhost:5000) + SocketCAN open. */
void monitor_service_env_ctor(void **env_out, char **bus, const char *name); /* 0x12fd10 */

/* vendor helpers used by main / service_env (modelled as opaque calls; the
 * full prototypes live in component.h, which this TU does not include). */
void component_reset_lines_ctor(void *self);                  /* the 4-GPIO bank */
void component_format_version_string(void *out);             /* build version str */
void component_mqtt_client_ctor(void *self, const char *name, const char *id,
                                int clean, const char *host, int port);

#endif /* MONITOR_MAIN_H */
