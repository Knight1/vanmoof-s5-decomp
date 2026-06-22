/*
 * devices/main/tracking/src/main.cpp  (reconstructed)
 *
 * VanMoof S5/A5 'tracking' service entry point.  ELF: tracking (AArch64, C++),
 * image base 0x100000.  OEM addresses are quoted per-function.
 *
 * tracking_main builds the service context (a ServiceEnv that connects to the
 * local MQTT broker / state bus over "localhost:5000" and opens SocketCAN),
 * then constructs the three application objects and runs the service until a
 * shutdown signal arrives:
 *
 *     Movement      -- IMU / motion observer (subscribes
 *                      "ux/tracking/alarm/imu/triggered", 30-min revert timer)
 *     CellLocator   -- cellular fix poller (0xd8 bytes), references Movement
 *     App           -- the 3-state theft state-machine (0x160 bytes), wires
 *                      the MQTT command subscriptions and spawns the worker
 *
 * The ServiceEnv plumbing (devices/main/common/src/service_env.cpp: the vm
 * handle, the IMQTTClient/state-client construction, the SocketCAN open, the
 * SIGINT/SIGTERM handlers and the "READY=1" sd_notify) is shared framework /
 * vendor code; it is modelled here through opaque service_env_* accessors and
 * is NOT reconstructed.  Only the tracking-authored object lifecycle and the
 * single main.cpp shutdown log line are reproduced.
 */

#include "tracking_common.h"

#define SRC "devices/main/tracking/src/main.cpp"

/* ================================================================== *
 *  tracking_main                                   OEM 0x00107c60
 *
 *  Equivalent C++ shape:
 *
 *      int tracking_main() {
 *          ServiceEnv env("tracking-service");
 *          Movement   movement(env.mqtt());
 *          auto      *locator = new CellLocator(env.mqtt(), movement);
 *          auto      *app     = new App(env.mqtt(), env.state(), locator);
 *          app->run();                  // blocks until shutdown
 *          env.notifyReady_/run_();
 *          delete app;
 *          // movement dtor + env dtor
 *          LOG(INFO, "TrackingService shut down");
 *          return 0;
 *      }
 * ================================================================== */
int tracking_main(void)
{
    ServiceEnv    env;            /* auStack_118 (ctx) + auStack_148 (handle) */
    Movement      movement;       /* auStack_e0 (216 bytes, by value)         */
    CellLocator  *locator;        /* plVar2  (new, 0xd8 bytes)                */
    TrackingApp  *app;            /* uVar4   (new, 0x160 bytes)               */
    void         *mqtt;
    void         *state;

    /* ServiceEnv env("tracking-service");
       FUN_0011ec30 builds the base context, FUN_0011f0f0 connects the MQTT /
       state client to localhost:5000 and opens SocketCAN. */
    service_env_ctor(&env);
    service_env_connect(&env, "tracking-service");

    /* Movement movement(env.mqtt());  -- constructed in-place by value. */
    mqtt = service_env_mqtt(&env);          /* FUN_0011e9f0: *(env+8)+0x10 */
    movement_ctor(&movement, mqtt);

    /* locator = new CellLocator(env.mqtt(), movement); */
    mqtt    = service_env_mqtt(&env);
    locator = (CellLocator *)operator_new(0xd8);
    cell_locator_ctor(locator, mqtt, &movement, NULL);

    /* app = new App(env.mqtt(), env.state(), &locator); */
    mqtt  = service_env_mqtt(&env);         /* FUN_0011e9f0 */
    state = service_env_state(&env);        /* FUN_0011ea00: *(env+0x20)   */
    app   = (TrackingApp *)operator_new(0x160);
    tracking_service_app_ctor(app, mqtt, state, &locator);

    /* app->run();  -- the OEM invokes vtable slot +8 on the CellLocator handle
       (local_150) which the App ctor takes by reference; it blocks until a
       shutdown signal flips the run flag. */
    if (locator != NULL)
        tracking_app_run(locator);

    /* env.notifyReady() / drain: sd_notify "READY=1" then wait for exit. */
    service_env_run(&env);                  /* FUN_0011e9b0 */

    /* Teardown, in reverse construction order. */
    tracking_service_app_delete(app);       /* deleting dtor, frees 0x160   */
    movement_dtor(&movement);
    service_env_destroy(&env);              /* FUN_0011e780: deinit vm/CAN  */

    common_logf(SRC, 0x20, LOG_WARN, "TrackingService shut down");  /* OEM level 3 = WARN */

    service_env_dtor(&env);                 /* FUN_0011e6f0: free ctx strings */
    return 0;
}