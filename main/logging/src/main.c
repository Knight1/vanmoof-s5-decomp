/*
 * main.c — VanMoof S5 i.MX8 `logging` service entry + CLI + service_env.
 *   OEM: devices/main/logging/src (main + logging_server.cpp), program
 *   "logging", AArch64, image base 0x100000, main @ 0x1074c0.
 *
 * Brings up the common service environment (vm_init, the MQTT client to
 * localhost:1883, the CAN socket, signal handlers for SIGINT/SIGILL/SIGABRT/
 * SIGTERM), constructs the LoggingService over it, runs it, then tears down.
 *
 * Behaviour-oriented C: the common ServiceEnv / MQTTClient / vm transport are
 * modelled as opaque externs (see logging.h); the CLI letters, defaults, help
 * strings and the optional [log_config.txt] positional are reproduced from the
 * 0x1074c0 decompilation.
 */
#include "logging.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>

/* ---- common ServiceEnv (vendor, modelled) ------------------------------ *
 * vm_init + common::MQTTClient(localhost:1883 keepalive 60) + SocketCAN open
 * + signal handlers {SIGINT, SIGILL, SIGABRT, SIGTERM}. */
typedef struct service_env service_env;
service_env *service_env_new(const char *name);                 /* FUN_001134e0 */
void         service_env_free(service_env *env);
mqtt_client *service_env_mqtt(service_env *env);                 /* FUN_00113290 */
vm_ctx      *service_env_can(service_env *env);                  /* FUN_001132c0 */
void         service_env_run(service_env *env);                  /* base run loop */

static void print_usage(const char *argv0)
{
    printf("Usage: %s [-s|--service] [-f|--filter <address>] [log_config.txt]\n", argv0);
    printf(" -s | --service  Run as a service\n");
    printf(" --filter|-f <address> Filter log messages to only show those from"
           " the given address (or device name)\n");
    printf(" [log_config.txt]: known strings, optional, defaults to the log"
           " configuration determined at service's build time\n");
}

int main(int argc, char **argv)
{
    bool         service_mode = false;     /* -s / --service */
    const char  *filter       = NULL;      /* -f / --filter <address> */
    const char  *config_path  = NULL;      /* optional [log_config.txt] positional */
    service_env *env;
    logging_service svc;
    int          c;

    static const struct option k_long[] = {
        { "service", no_argument,       0, 's' },
        { "filter",  required_argument, 0, 'f' },
        { 0, 0, 0, 0 },
    };

    while ((c = getopt_long(argc, argv, "sf:h", k_long, NULL)) != -1) {
        switch (c) {
        case 's':
            service_mode = true;
            break;
        case 'f':
            filter = optarg;               /* address or device name */
            break;
        case 'h':
        case '?':
        default:
            print_usage(argv[0]);          /* ServiceEnv help-required path */
            return 1;
        }
    }

    /* optional single positional: the log_config.txt path */
    if (optind + 1 == argc)
        config_path = argv[optind];

    /* service environment on "logging" (vm + MQTT localhost:1883 + CAN). */
    env = service_env_new("logging");

    /* construct + run the LoggingService, then tear down. */
    logging_service_init(&svc, service_env_mqtt(env), service_env_can(env),
                         NULL /* std::cout */, config_path, service_mode, filter);
    service_env_run(env);
    logging_service_run(&svc);
    logging_service_deinit(&svc);

    service_env_free(env);
    return 0;
}
