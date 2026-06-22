/*
 * main.c — UX service program entry.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000), function
 * main @0x10ede0 (carved from the _INIT_1 blob), source
 * devices/main/ux/src/main.cpp.
 *
 * main() registers a single CLI option ("-p | --path  Override storage path"),
 * builds the "ux-service" application framework, pulls the three message-bus/
 * config handles out of it, constructs the UXService over them, runs it
 * (blocks until shutdown is signalled), then tears everything down. The app
 * framework + arg parser are vendor (common::App); modelled opaquely.
 */
#include "ux_common.h"
#include "main.h"

/* The UXService is the largest object in the image (>0x1098 bytes); it is
 * placement-constructed into this stack buffer in the OEM (auStack_11e0 holds
 * the app, local_11a8 the service). We keep a sufficiently large aligned
 * backing store. */
static ux_service *ux_service_storage(void)
{
    /* placeholder backing store for the placement-constructed UXService */
    static _Alignas(16) unsigned char buf[0x10a0];
    return (ux_service *)buf;
}

int main(int argc, char **argv)
{
    ux_cfg     cfg;     /* auStack_1208 : storage/config */
    ux_app     app;     /* auStack_11e0 : app framework   */
    ux_opts    opts;    /* auStack_1298 : parsed options  */
    ux_service *svc = ux_service_storage(); /* local_11a8 */
    void *bus_a, *bus_b, *cfg_handle;

    ux_cfg_init(&cfg, 0);
    ux_app_init(&app);

    /*
     * Build the option vector. The OEM hand-builds a one-entry option list:
     *   { "-p | --path  Override storage path" } with arg-count 1, callback
     *   FUN_00117390/FUN_001173d0 closing over the cfg object, then parses argv
     *   into it. Modelled as a single parse_args call.
     */
    ux_app_parse_args(&app, argc, argv,
                      /*name*/   "ux-service",
                      /*opt   */ (void *)" -p | --path  Override storage path",
                      /*help  */ NULL,
                      /*cb_cfg*/ &cfg);

    /* build the framework, then extract the three handles UXService needs */
    ux_app_build(&opts, &app, "ux-service");
    bus_a      = ux_app_bus_a(&opts);   /* FUN_00182ef0 */
    bus_b      = ux_app_bus_b(&opts);   /* FUN_00182f00 */
    cfg_handle = ux_app_cfg(&opts);     /* FUN_00182ee0 */

    ux_service_ctor(svc, bus_a, bus_b, cfg_handle, &cfg);
    ux_service_run(svc);

    common_logf("devices/main/ux/src/main.cpp", 0x24, LOG_WARN, "UX Service started");

    ux_app_run(&opts); /* blocks until shutdown signalled */

    common_logf("devices/main/ux/src/main.cpp", 0x27, LOG_WARN, "Shutting down UX Service");
    ux_service_dtor(svc);
    common_logf("devices/main/ux/src/main.cpp", 0x2d, LOG_WARN, "UX shut down");

    ux_app_stop(&opts);
    ux_app_destroy(&app);
    ux_cfg_destroy(&cfg);
    return 0;
}
