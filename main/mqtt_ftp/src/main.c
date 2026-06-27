/*
 * main.c — VanMoof S5 i.MX8 `mqtt-ftp-service` entry + CLI + service_env.
 *   OEM: devices/main/mqtt-ftp/src/main.cpp, program "mqtt-ftp-service",
 *   AArch64, image base 0x100000, main @ 0x107850.
 *
 * Parses the CLI (a mandatory <destination-folder> and an optional <chunk-size>,
 * default 512), brings up the common service environment (vm_init + MQTT to
 * localhost:1883 + IClock), constructs the MqttFtpService over it, runs it, then
 * tears down.  The common ServiceEnv / MQTTClient / IClock are modelled as
 * opaque externs; the option letters, the chunk-size default and the help/log
 * strings are reproduced from the 0x107850 decompilation.
 */
#include "mqtt_ftp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* ---- common ServiceEnv (vendor, modelled) ------------------------------ *
 * vm_init + common::MQTTClient(localhost:1883 keepalive 60) + IClock. */
typedef struct service_env service_env;
service_env *service_env_new(const char *name);
void         service_env_free(service_env *env);
mqtt_client *service_env_mqtt(service_env *env);
clock_iface *service_env_clock(service_env *env);
void         service_env_run(service_env *env);

static void print_usage(const char *argv0)
{
    printf("Usage: `%s` [params...] <destination-folder> <chunk-size>\n", argv0);
    printf("params:\n");
    printf(" -v | --verbose         Enable verbose log output\n");
    printf(" <destination-folder>: folder used to write files to, mandatory\n");
    printf(" <chunk-size>: chunk file used, file-transfer is split up in chunks,"
           " defaults to %d\n", 0x200);
}

int main(int argc, char **argv)
{
    bool         verbose    = false;
    const char  *dest       = NULL;
    uint32_t     chunk_size = 0x200;       /* 512, the default */
    service_env *env;
    mqtt_ftp_service svc;
    int          c;

    static const struct option k_long[] = {
        { "verbose", no_argument, 0, 'v' },
        { 0, 0, 0, 0 },
    };

    while ((c = getopt_long(argc, argv, "vh", k_long, NULL)) != -1) {
        switch (c) {
        case 'v':
            verbose = true;
            break;
        case 'h':
        case '?':
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* <destination-folder> is mandatory; <chunk-size> is the optional 2nd arg. */
    if (optind >= argc) {
        print_usage(argv[0]);              /* ServiceEnv help-required path */
        return 1;
    }
    dest = argv[optind];
    if (optind + 1 < argc)
        chunk_size = (uint32_t)strtoul(argv[optind + 1], NULL, 10);

    env = service_env_new("mqtt-ftp-service");

    mqtt_ftp_service_init(&svc, service_env_mqtt(env), dest,
                          service_env_clock(env), chunk_size);
    svc.verbose = verbose;
    service_env_run(env);
    mqtt_ftp_service_run(&svc);
    mqtt_ftp_service_deinit(&svc);

    common_logf("devices/main/mqtt-ftp/src/main.cpp", 0x2f, LOG_WRN,
                "UpdateService shut down");   /* OEM string (copy-paste artifact) */
    service_env_free(env);
    return 0;
}
