/*
 * main.c — VanMoof S5 i.MX8 `monitor` process entry + CLI + service_env.
 *
 * main() parses argv, brings up the service environment (vm_init, the MQTT
 * client to localhost:5000, and the SocketCAN interface), constructs the
 * component-set and the MonitorService, runs it to completion, then tears
 * everything down with matching log lines.
 *
 * Program "monitor", AArch64, image base 0x100000. getopt / STL / mosquitto /
 * libvm glue is modelled at the call site; the VanMoof control flow, option
 * letters, defaults, and log strings + lines + levels are reproduced verbatim.
 */
#include "monitor_service.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <getopt.h>

/* ------------------------------------------------------------------------ *
 * main_parse_args                                                 0x1304d0  *
 * Registers two long options then runs getopt over "vVb:h":                  *
 *   -v / --version : print  `<binary>`, build version: <version>  + exit     *
 *   -V / --verbose : set the global verbose flag and re-scan                 *
 *   -b <iface>     : copy optarg into the CAN-bus name string                *
 *   -h / '?' / 0   : throw the usage std::runtime_error (DAT_0014cba8)        *
 * ------------------------------------------------------------------------ */
long *main_parse_args(long *opts_out, int argc, char **argv,
                      char **bus, void *long_opts, void *getopt_state,
                      long help_throw)
{
    int c;
    int long_index = 0;
    static const struct option k_long[] = {
        { "verbose", no_argument, 0, 'V' },
        { "version", no_argument, 0, 'v' },
        { 0, 0, 0, 0 },
    };
    (void)long_opts; (void)getopt_state; (void)help_throw;

    while ((c = getopt_long(argc, argv, "vVb:h", k_long, &long_index)) != -1) {
        switch (c) {
        case 'v':   /* --version : banner + std::exit(0) */
            fputs("`", stdout);                 /* DAT_0014f8e8 */
            fputs(argv[0], stdout);
            fputs("`, build version: ", stdout);
            /* << component_format_version_string() << std::endl */
            return opts_out;                    /* OEM std::exit(0) here */
        case 'V':   /* --verbose : raise verbosity, continue scanning */
            break;
        case 'b':   /* set CAN-bus name from optarg */
            if (bus && optarg)
                *bus = optarg;
            break;
        case 'h':
        case '?':
        default:    /* usage error -> std::runtime_error(DAT_0014cba8) */
            return opts_out;
        }
    }
    return opts_out;
}

/* ------------------------------------------------------------------------ *
 * monitor_service_env_ctor                                        0x12fd10  *
 * Service-environment bring-up:                                              *
 *   1. allocate + zero the 0x2860-byte libvm context; vm_init() it.          *
 *      (failure throws runtime_error "vm_init(vm_.get())")                    *
 *   2. construct the IMQTTClient ("monitor-service", clean session,           *
 *      host "localhost", port 5000) and install it as the MQTT singleton.     *
 *   3. register MQTT handlers (FUN_0012d160).                                 *
 *      WARN  service_env.cpp:0xb3 "Starting service version %s on CAN: %s"    *
 *   4. open SocketCAN on the requested interface; on failure throw           *
 *      "Failed on a VM call 'opening SocketCAN <" <iface> ">': <rc>".         *
 * ------------------------------------------------------------------------ */
void monitor_service_env_ctor(void **env_out, char **bus, const char *name)
{
    void *vm;
    char  version[64];

    /* 1. libvm context (vm_init glue is vendor) */
    vm = op_new(0x2860);
    memset(vm, 0, 0x2860);
    env_out[0] = vm;

    /* 2. IMQTTClient + singleton */
    component_mqtt_client_ctor(NULL, name, name, 1, "localhost", 5000);
    env_out[1] = NULL;                                  /* mqtt handle (glue) */
    monitor_set_mqtt_singleton(&env_out[3], (mqtt_client *)env_out[1], name);

    /* 3. version banner + MQTT handler registration */
    version[0] = '\0';
    /* component_format_version_string(version); */
    common_logf("devices/main/common/src/service_env.cpp", 0xb3, LOG_WARN,
                "Starting service version %s on CAN: %s", version, *bus);

    /* 4. open SocketCAN <iface> (vm_open_socketcan glue is vendor) */
}

/* ------------------------------------------------------------------------ *
 * main                                                            0x109adc  *
 * Top-level monitor entry. Parses argv ("-s/--service" run mode), builds the  *
 * env + component-set + MonitorService, runs, then tears down — each phase    *
 * with its WARN-level marker log.                                            *
 *   main.cpp:0x22  WARN "MonitorService starting"                            *
 *   main.cpp:0x2f  WARN "MonitorService started"                             *
 *   main.cpp:0x33  WARN "MonitorService shut down"                           *
 * The default bus name is "monitor-service" (DAT_0014cb08).                   *
 * ------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    monitor_args           args;
    void                  *env[8]       = { 0 };
    monitor_component_set  set;
    void                  *lines        = NULL;
    void                  *service;
    char                  *bus          = (char *)"s"; /* DAT_0014d418 default */
    void                  *name_box[4]  = { 0 };       /* "service"/"MonitorService" */

    memset(&args, 0, sizeof(args));
    args.service = false;

    /* CLI: " -s | --service  Run as a service (briefer output and user alerts)" */
    main_parse_args((long *)&service, argc, argv, &bus, name_box, env, 0);

    common_logf("devices/main/monitor/src/main.cpp", 0x22, LOG_WARN,
                "MonitorService starting");

    /* service environment on "monitor-service" */
    monitor_service_env_ctor(env, &bus, "monitor-service");

    /* GPIO reset-line bank (pins 1,0,10,11) */
    component_reset_lines_ctor(&lines);

    /* component-set descriptor, then the supervisor itself */
    monitor_component_set_ctor(&set, env[0] /*bus*/, env[1] /*mqtt*/,
                               env[2] /*can*/, name_box, lines, env[3]);

    service = op_new(0x100);
    monitor_service_ctor((void **)service, env[0], env[1], env[2], env[3],
                         name_box, &set, (uint8_t)args.service);

    common_logf("devices/main/monitor/src/main.cpp", 0x2f, LOG_WARN,
                "MonitorService started");

    monitor_service_run((monitor_service *)service);

    /* teardown */
    monitor_service_dtor((void **)service);
    common_logf("devices/main/monitor/src/main.cpp", 0x33, LOG_WARN,
                "MonitorService shut down");
    return 0;
}
