/*
 * devices/main/power/src/power_service.cpp  (reconstructed)
 *
 * VanMoof S5/A5 'power' service top level.  ELF: /usr/bin/power (AArch64, C++).
 * Image base 0x100000.  OEM addresses are quoted per-function.
 *
 * STL / std::function / std::string / vm plumbing is modelled through the
 * shared interfaces: an MQTT subscription is od_subscribe(ipc, topic, handler,
 * ctx); a periodic timer is make_periodic_timer(factory, ms, handler, ctx) and
 * is driven through its small vtable (timer_arm / timer_stop / timer_running).
 * The original ctor open-codes the std::string topic construction and the
 * std::function trampolines inline; that boilerplate is folded into the helper
 * calls below so the behaviour, not the allocator dance, is preserved.
 */

#define _DEFAULT_SOURCE
#include <stdlib.h>
#include "power_service.h"

#include "power_common.h"

#define SRC "devices/main/power/src/power_service.cpp"

/* ------------------------------------------------------------------ *
 *  PS_substate (+0x120) state encoding of the standby sub-machine.
 * ------------------------------------------------------------------ */
enum standby_substate {
    kStandbyEnter        = 0,  /* (re)issue standby switches, battery off if charger gone */
    kStandbyLiPoCharging = 1,  /* buck enabled, charging the LiPo */
    kStandbyGoToSleep    = 2,  /* ask battery/LPC/nRF to go to standby */
    kStandbyCheckCanQuiet= 3,  /* verify the CAN bus is quiet */
    kStandbySuspend      = 4,  /* suspend the SoC (or stay awake if VAC present) */
};

/* ================================================================== *
 *  main                                            OEM 0x0010a220
 * ================================================================== */
int main(int argc, char **argv)
{
    ServiceEnv env;
    PowerService *ps;

    /* ServiceEnv env("power-service", argc, argv); */
    service_env_init(&env, argc, argv, "power-service", 0);

    /* The ctor takes the four framework singletons the env vends. */
    ps = (PowerService *)operator_new(0x448);
    PowerService_ctor(ps,
                      service_env_ipc(&env),       /* +0x14 .. message bus     */
                      service_env_clock(&env),     /* +0x118 monotonic clock   */
                      service_env_timerfactory(&env),
                      service_env_config(&env));

    common_logf("devices/main/power/src/main.cpp", 0x19, LOG_INFO,
                "Power service started");
    PowerService_Run(ps);

    service_env_run(&env);            /* FUN_001402b0: drain the bus */
    common_logf("devices/main/power/src/main.cpp", 0x21, LOG_INFO,
                "Power service shutting down");
    PowerService_Stop(ps);
    common_logf("devices/main/power/src/main.cpp", 0x23, LOG_INFO,
                "Power service shut down");

    service_env_destroy(&env);
    return 0;
}

/* ================================================================== *
 *  Construction                                    OEM 0x00116350
 *
 *  Lays out the 0x448-byte PowerService and wires:
 *   +0x50   PowerControl
 *   +0x118  StateManager base (clock, requested/current state, mutex +0xe8)
 *   +0x120  PS_substate, +0x121 charger flag
 *   +0x128  LiPo / BQ25672 gauge control
 *   +0x150  Monitor
 *   +0x1f0  tick timer (4000ms factory, charge_supervisor_worker)
 *   +0x1f8  standby low-power timer (50000000-style one-shot)
 *   +0x200  fast 50ms poll timer (switch_control timer)
 *   +0x208  switch_control
 *   +0x3a0  low_power
 *   +0x360..0x398 charge-cycle bookkeeping
 *   +0x408  od/config handle, +0x410 charge counters
 * ================================================================== */
void PowerService_ctor(PowerService *self, void *ipc, void *clock,
                       void *timerfactory, void *config)
{
    uint8_t *s = (uint8_t *)self;

    /* StateManager base ctor (installs the state machine + mutex at +0xe8). */
    StateManager_ctor(self, timerfactory, config);

    /* The state-machine "set state" callback is bound to this PowerService
     * and dispatches through the vtable (slots +0x10..+0x40) below. */
    state_manager_bind_on_state(self + 0x118, (ps_cb)&PowerService_OnStateChanged, self);

    PS_state_vtable(self)     = (void *)&DAT_00184a88;   /* *self        */
    state_manager_vtable(self)= (void *)&DAT_00184b20;   /* self[10]     */

    /* zero the OdRegistration / std::function slots (self[0x1d..0x22]) */
    for (int i = 0x1d; i <= 0x22; i++)
        ((void **)self)[i] = 0;
    ((void **)self)[0x23] = clock;                       /* +0x118 clock */
    *(uint16_t *)(s + 0x120) = 0x0300;                   /* substate=0, flags=3 */

    /* PowerControl(+0x50): 30 retries / 80 ms. */
    PowerControl_ctor(s + 0x50, 0x1e, 0x50);
    lipo_ctor(s + 0x128);                                /* self[0x28] */

    Monitor_ctor(s + 0x150, ipc, clock, timerfactory, s + 0x128);

    ((void **)self)[0x3e] = 0;   /* tick timer        (+0x1f0) */
    ((void **)self)[0x3f] = 0;   /* standby lp timer  (+0x1f8) */
    ((void **)self)[0x40] = 0;   /* 50ms poll timer   (+0x200) */
    *(uint8_t *)(s + 0x394) = 0;

    /* switch_control(+0x208) -- consumes the clock to schedule the 50ms poll. */
    switch_control_ctor(s + 0x208, ipc, s + 0x140, clock);
    ((void **)self)[0x81] = config;
    low_power_ctor(s + 0x3a0, config, clock, timerfactory);

    /* ----- subscription table ------------------------------------ *
     * Each entry: subscribe(topic) -> bound member trampoline.       */
    od_subscribe(clock, "power/low_power_extend",        /* len 0x16 */
                 (ps_cb)&on_mqtt_low_power_extend, self);
    od_subscribe(clock, "modem/system/time",            /* len 0x11 */
                 (ps_cb)&on_mqtt_modem_time, self);

    /* StateManager owns power/state/set + power/state/extend_timeout; the
     * config bus owns maintenance/battery/primary/reset.  The ctor binds
     * the state-machine "request" handler (self[0x3f] reset slot) here. */
    state_manager_subscribe_requests(config, (ps_cb)&PowerService_OnStateChanged, self);

    od_subscribe(clock, "maintenance/battery/primary/reset",  /* len 0x21 */
                 (ps_cb)&on_mqtt_battery_reset, self);

    /* ----- timers ------------------------------------------------- *
     * 4000ms periodic tick -> charge_supervisor_worker; armed now.    */
    ((void **)self)[0x3e] =
        make_periodic_timer(timerfactory, 4000,
                            (ps_cb)&timer_4000ms_charge_supervisor, self);
    timer_arm(((void **)self)[0x3e], 4000);

    /* 50ms periodic poll -> timer_50ms_poll (switch_control fast path),
     * created but left disarmed; armed in Run()/TurnOn(). */
    ((void **)self)[0x40] =
        make_periodic_timer_fast(timerfactory, 50,
                                (ps_cb)&timer_50ms_poll_trampoline, self);
    timer_stop(((void **)self)[0x40]);
}

/* PowerService::OnStateChanged(state) - dispatch to the vtable slot for a
 * requested power_state.  OEM: the StateManager calls *self+0x10..+0x40
 * keyed by (state-1).  Modelled explicitly so the table stays visible. */
void PowerService_OnStateChanged(PowerService *self, int state)
{
    switch ((enum power_state)state) {
    case ST_SHIPPING:    PowerService_OnShipping(self);    break;
    case ST_STANDBY:     PowerService_OnStandby(self);     break;
    case ST_OPERATIONAL: PowerService_OnOperational(self); break;
    case ST_CHARGING:    PowerService_OnCharging(self);    break;
    case ST_UPDATING:    PowerService_OnUpdating(self);    break;
    case ST_ALARM:       PowerService_OnAlarm(self);       break;
    case ST_MAINTENANCE: PowerService_OnMaintenance(self); break;
    default:                                               break;
    }
}

/* ================================================================== *
 *  Run                                             OEM 0x00116e50
 *  - push the standby register profile to the BQ25672
 *  - prime the 50ms poll
 *  - if charger present on entry -> go to CHARGING, else -> standby
 *  - start the StateClient and block until shutdown
 * ================================================================== */
void PowerService_Run(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    struct timespec ts;

    lipo_bq25672_write_regs_standby(s + 0x128, 0, 0);
    timer_50ms_poll(self);                       /* one immediate poll */

    /* nanosleep(1s 500ms) settle */
    ts.tv_sec = 1; ts.tv_nsec = 500000000;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }

    if (power_control_charger_connected(s + 0x50)) {
        switch_control_set_state(s + 0x208, ST_CHARGING);
        StateManager_ChangeState(self, ST_CHARGING);
    } else {
        PowerService_OnStandby(self);
    }

    state_client_start(self);                    /* FUN_0011a7e0: blocks */
    state_client_join(s + 0x50);                 /* FUN_001332d0 */
}

/* ================================================================== *
 *  Stop                                            OEM 0x00116e10
 * ================================================================== */
void PowerService_Stop(PowerService *self)
{
    PowerService_dtor(self);                     /* FUN_00116b60 */
    operator_delete(self, 0x448);
}

/* PowerService::~PowerService()                    OEM 0x00116b60
 * Restores base vtables, unsubscribes power/low_power_extend +
 * modem/system/time, stops/destroys the three timers, then tears down
 * switch_control / low_power / monitor / the subscription registrations
 * and the StateManager base. */
void PowerService_dtor(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;

    state_manager_vtable(self) = (void *)&DAT_00184b20;
    PS_state_vtable(self)      = (void *)&DAT_00184a88;

    od_unsubscribe(((void **)self)[0x23], "power/low_power_extend");
    od_unsubscribe(((void **)self)[0x23], "modem/system/time");

    if (timer_running(((void **)self)[0x3e])) timer_stop(((void **)self)[0x3e]);
    if (timer_running(((void **)self)[0x3f])) timer_stop(((void **)self)[0x3f]);
    if (timer_running(((void **)self)[0x40])) timer_stop(((void **)self)[0x40]);

    low_power_dtor(s + 0x3a0);
    switch_control_dtor(s + 0x208);

    timer_release(((void **)self)[0x40]);
    timer_release(((void **)self)[0x3f]);
    timer_release(((void **)self)[0x3e]);

    Monitor_dtor(s + 0x150);
    lipo_dtor(s + 0x128);
    PowerControl_dtor(s + 0x50);
    StateManager_dtor(self);
}

/* ================================================================== *
 *  TurnOn                                          OEM 0x00112c40
 *  Bring the primary battery up and start the 50ms power-on sequence.
 * ================================================================== */
void PowerService_TurnOn(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;

    common_logf(SRC, 0x218, LOG_INFO, "Turn on");
    common_logf(SRC, 0x219, LOG_INFO,
                "IsPrimaryInserted: %d BatteryINS-DET: %d BatteryINS-DET FAIL: %d",
                get_is_primary_inserted(s + 0x50),
                monitor_battery_ins_det(s + 0x150),
                monitor_battery_ins_det_fail(s + 0x150));

    low_power_publish_low_power(s + 0x3a0);          /* FUN_00125030 */
    low_power_request_can_low_power(s + 0x3a0);      /* FUN_00123940 */

    *(uint8_t *)(s + 0x121) = 0;
    *(uint8_t *)(s + 0x391) = 0;

    switch_control_power_on(s + 0x200);              /* fast 50ms timer +0x18 */
    common_logf(SRC, 0x220, LOG_INFO, "Waiting for battery to power on");

    *(uint64_t *)(s + 0x368) = clock_now_ns();       /* FUN_00109080 */
    battery_cmd1_on(s + 0x50);
    powerservice_refresh_clock_file();               /* FUN_00112870 */
}

/* ================================================================== *
 *  State handlers (PowerService vtable slots +0x10..+0x40).
 *  Each takes PS_mutex (+0xe8) on entry and releases it on exit.
 * ================================================================== */

/* OnShipping                                       OEM 0x00112720 */
void PowerService_OnShipping(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    StateManager_ChangeState(self, ST_SHIPPING);
    timer_arm(((void **)self)[0x3e], 3000);          /* tick re-arm */

    ps_mutex_unlock(s + 0xe8);
}

/* OnStandby                                        OEM 0x00112980 */
void PowerService_OnStandby(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x231, LOG_INFO, "To standby state");
    *(uint8_t  *)(s + 0x120) = kStandbyEnter;        /* PS_substate */
    *(uint32_t *)(s + 0x394) = 0;                    /* LiPo CheckTimes */

    StateManager_ChangeState(self, ST_STANDBY);
    timer_arm(((void **)self)[0x3e], 3000);
    powerservice_refresh_clock_file();               /* touch + sync */

    ps_mutex_unlock(s + 0xe8);
}

/* OnOperational                                    OEM 0x00112d30 */
void PowerService_OnOperational(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x244, LOG_INFO, "To operational state");
    if (power_control_charger_connected(s + 0x50)) {
        common_logf(SRC, 0x24a, LOG_ERR,
                    "Charger connected. Not allowed to power on the system");
    } else {
        timer_stop(((void **)self)[0x3e]);
        if (monitor_primary_capacity(s + 0x150) == 0) {
            common_logf(SRC, 0x251, LOG_WARN, "Primary Capacity is too low");
        } else {
            PowerService_TurnOn(self);
        }
    }

    ps_mutex_unlock(s + 0xe8);
}

/* OnCharging                                       OEM 0x001127c0 */
void PowerService_OnCharging(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x25c, LOG_INFO, "To charging state");
    timer_stop(((void **)self)[0x3e]);
    charge_counters_reset(s + 0x410);                /* FUN_0011fed0 */

    ps_mutex_unlock(s + 0xe8);
}

/* OnUpdating                                       OEM 0x00112e40 */
void PowerService_OnUpdating(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x269, LOG_INFO, "To updating state");
    timer_stop(((void **)self)[0x3e]);
    if (StateManager_CurrentState(self) == ST_CHARGING)
        StateManager_ChangeState(self, ST_UPDATING);
    else
        PowerService_TurnOn(self);

    ps_mutex_unlock(s + 0xe8);
}

/* OnAlarm                                          OEM 0x00112f10 */
void PowerService_OnAlarm(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x276, LOG_INFO, "To alarm state");
    timer_stop(((void **)self)[0x3e]);
    PowerService_TurnOn(self);

    ps_mutex_unlock(s + 0xe8);
}

/* OnMaintenance                                    OEM 0x00112440 */
void PowerService_OnMaintenance(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    ps_mutex_lock(s + 0xe8);

    common_logf(SRC, 0x27e, LOG_INFO, "To Maintenance state");
    timer_stop(((void **)self)[0x3e]);   /* tick  */
    timer_stop(((void **)self)[0x40]);   /* 50ms  */

    if (!power_control_battery_is_off(s + 0x50)) {
        common_logf(SRC, 0x283, LOG_INFO,
                    "Turning battery off before entering Maintenance state");
        battery_cmd0_off(s + 0x50);
    }

    switch_control_set_state(s + 0x208, ST_MAINTENANCE);  /* state 7 */
    StateManager_ChangeState(self, ST_MAINTENANCE);

    ps_mutex_unlock(s + 0xe8);
}

/* ================================================================== *
 *  MQTT handlers (bound member trampolines).
 * ================================================================== */

/* on_mqtt_state_set : power/state/set        OEM 0x0011c3a0 */
void on_mqtt_state_set(PowerService **ctx, void *topic, void *meta, void *json)
{
    int state = 0;
    (void)topic; (void)meta;
    od_parse_int(json, &state, 0);
    StateManager_OnStateRequest(*ctx, state, 0);
}

/* on_mqtt_state_extend_timeout : power/state/extend_timeout  OEM 0x0011c320 */
void on_mqtt_state_extend_timeout(PowerService **ctx, void *topic, void *meta,
                                  void *json)
{
    StateManager *sm = (StateManager *)*ctx;
    int secs = 0;
    (void)topic; (void)meta;
    od_parse_int(json, &secs, 0);
    StateManager_SetExtendTimeout(sm, secs);     /* vtable +0x60 */
}

/* on_mqtt_low_power_extend : power/low_power_extend   OEM 0x00116280 */
void on_mqtt_low_power_extend(PowerService **ctx, void *topic, void *meta,
                              void *json)
{
    int secs;
    (void)topic; (void)meta;

    /* Only number-like JSON nodes (tag in {5,6,7}) carry an extension. */
    if (!od_is_number(json))
        return;

    secs = 0;
    od_parse_int_signed(json, &secs);
    common_logf(SRC, 0x56, LOG_INFO,
                "Extending standby timeout with %d seconds", secs);

    secs = 0;
    od_parse_uint(json, &secs);
    /* standby low-power timer (+0x1f8): arm for secs*1000 ms. */
    timer_arm((*ctx)->_raw + 0x1f8 == 0 ? 0 : ((void **)*ctx)[0x3f],
              (uint64_t)secs * 1000);
}

/* on_mqtt_modem_time : modem/system/time     OEM 0x00115ad0
 *
 * Expects {"ret":1, "time":<epoch>}.  When ret==1 and "time" is present, and
 * the service is in STANDBY (not mid-transition, power-on-seq timer idle),
 * the epoch is pushed to the RTC/clock file via the state machine (+0x140)
 * and the low_power handle (+0x408), then the clock file is refreshed.
 * Otherwise it logs that it won't update the time, or a RequestTimeError. */
void on_mqtt_modem_time(PowerService **ctx, void *topic, void *meta,
                        void *json, void *meta2, const char *err)
{
    PowerService *self = *ctx;
    uint8_t *s = (uint8_t *)self;

    (void)topic; (void)meta; (void)meta2;
    if (*err != '\0' || !od_obj_get_bool(json, "ret"))   /* ret != 1 */
        return;

    if (od_obj_has(json, "ret") && od_obj_get_bool(json, "ret")) {
        if (od_obj_has(json, "time")) {
            char timestr[32];
            od_obj_get_string(json, "time", timestr, sizeof timestr);

            /* Only when standby, settled, and the power-on sequence timer
             * (switch_control +0x28 IsRunning) is idle. */
            if (StateManager_CurrentState(self) == ST_STANDBY &&
                !StateManager_InTransition(self) &&
                !switch_control_power_on_seq_running(s + 0x200)) {
                state_manager_set_clock(s + 0x140, timestr);   /* FUN_0011a080 */
                rtc_set_time(((void **)self)[0x408], timestr);
                powerservice_refresh_clock_file();
            } else {
                char req[32], cur[32];
                common_logf(SRC, 0x67, LOG_INFO,
                    "Not updating time because we are not in standby or in a state transition");
                StateManager_RequestedStateName(self, req);
                StateManager_StateName(self, cur);
                common_logf(SRC, 0x68, LOG_INFO,
                    "RequestedState: %s CurrentState: %s power_on_seq_timer->IsRunning: %d",
                    req, cur,
                    switch_control_power_on_seq_running(s + 0x200));
            }
            return;
        }
    }

    /* malformed payload */
    {
        int rterr = 0;
        od_obj_get_int(json, "error", &rterr);
        common_logf(SRC, 0x6e, LOG_ERR, "RequestTimeError: %d", rterr);
    }
}

/* on_mqtt_battery_reset : maintenance/battery/primary/reset  OEM 0x00112a50
 * Reset the primary battery, wait ~16s, then drop to standby. */
void on_mqtt_battery_reset(PowerService **ctx, void *topic, void *meta,
                           void *json)
{
    PowerService *self = *ctx;
    struct timespec ts;
    (void)topic; (void)meta; (void)json;

    common_logf(SRC, 0x7c, LOG_INFO, "MQTT Request to Reset Primary Battery.");
    battery_cmd6_reset((uint8_t *)self + 0x50);

    ts.tv_sec = 0x10; ts.tv_nsec = 0;             /* 16 s */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }

    common_logf(SRC, 0x7f, LOG_INFO,
                "Primary Battery Reset completed - going to Standby.");
    PowerService_OnStandby(self);
}

/* ================================================================== *
 *  charge_supervisor_worker                        OEM 0x00115140
 *  Driven by the 4000ms tick timer.  Runs the standby sub-state machine
 *  on PS_substate (+0x120) and re-arms the tick with a state-dependent
 *  period.  Counters:
 *    +0x360  charge elapsed (double s)   +0x368  power-on start ns
 *    +0x380  charge start ns             +0x388  charge end ns
 *    +0x394  LiPo CheckTimes             +0x398  CAN-quiet retries
 *    +0x390  standalone-shipping flag
 * ================================================================== */
void charge_supervisor_worker(PowerService *self)
{
    uint8_t *s = (uint8_t *)self;
    uint8_t *lipo  = s + 0x128;
    uint8_t *pc    = s + 0x50;
    uint8_t *mon   = s + 0x150;
    uint8_t *lp    = s + 0x3a0;
    uint8_t *sw    = s + 0x208;
    uint64_t rearm;
    struct timespec ts;

    /* --- prelude: charger-in-standby drives the gauge / battery off --- */
    if (charger_in_standby(self)) {                /* FUN_00112fc0 */
        common_logf(SRC, 0x154, LOG_INFO, "Charger connected while in standby");
        timer_arm(((void **)self)[0x3e], 30000);
        return;
    }

    if (lipo_charger_present(lipo)) {              /* FUN_00121ce0 */
        /* battery off OR INS-DET set -> try to turn on for LiPo charging. */
        if (!power_control_charger_off(pc) ||
            monitor_battery_ins_det(mon)) {
            int retries = 20;
            PowerService_TurnOn(self);
            for (;;) {
                if (power_control_charger_off(pc) == 0 &&
                    power_control_battery_up(pc)) {        /* FUN_001333f0 */
                    common_logf(SRC, 0x160, LOG_INFO,
                                "Enable Buck for charging lipo");
                    buck_set_enable(sw, 1);
                    ts.tv_sec = 0; ts.tv_nsec = 100000000; /* 100 ms */
                    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
                    break;
                }
                ts.tv_sec = 0; ts.tv_nsec = 250000000;     /* 250 ms */
                while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
                if (--retries == 0)
                    break;
            }

            if (*(uint8_t *)(s + 0x120) != kStandbyLiPoCharging)
                *(uint64_t *)(s + 0x380) = clock_now_ns();   /* stamp start */
            *(uint8_t *)(s + 0x120) = kStandbyLiPoCharging;
            switch_control_set_state(sw, ST_STANDBY);        /* state 2 */
        }
    }

    common_logf(SRC, 0x171, LOG_INFO, "LiPo Capacity: %d Current: %d",
                lipo_capacity(lipo), lipo_current(lipo));

    if (lipo_capacity(lipo) == 0 && lipo_is_dead(lipo) &&
        lipo_current(lipo) < 1) {
        common_logf(SRC, 0x173, LOG_INFO,
            "LiPo capacity is 0 and status reported as Dead, resetting fuel gauge");
        low_power_reset_fuel_gauge(lp);
        ts.tv_sec = 0x14; ts.tv_nsec = 0;          /* 20 s */
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    }

    /* ------------------------- sub-state switch ------------------------- */
    switch ((enum standby_substate)*(uint8_t *)(s + 0x120)) {

    case kStandbyEnter: {                          /* 0 */
        switch_control_apply_standby(sw, ST_STANDBY);            /* state 2 */
        if (!power_control_battery_is_off(pc))
            battery_cmd0_off(pc);
        *(uint8_t *)(s + 0x120) = kStandbyGoToSleep;
        rearm = 10000;
        break;
    }

    case kStandbyLiPoCharging: {                   /* 1 */
        double elapsed;
        *(uint64_t *)(s + 0x388) = clock_now_ns();
        elapsed = (double)(*(int64_t *)(s + 0x388) -
                           *(int64_t *)(s + 0x380)) / 1000000000.0;
        *(double *)(s + 0x360) = elapsed;
        common_logf(SRC, 0x189, LOG_INFO,
            "kStandbyLiPoCharging for %d minutes %d seconds",
            (int)elapsed / 60, (int)elapsed % 60);

        if ((int)*(double *)(s + 0x360) >= 3600) {
            common_logf(SRC, 0x18c, LOG_INFO,
                "Maximum lipo charge duration is reached, going back to standby");
            lipo_charge_complete(lipo);            /* FUN_00122440 */
            *(uint8_t *)(s + 0x120) = kStandbyEnter;
            rearm = 3000;
            break;
        }

        /* standalone main ECU (no primary up / no INS-DET): go to shipping. */
        if (power_control_battery_up(pc) == 0 &&
            monitor_standalone(mon) == 0) {        /* FUN_00127ec0 */
            common_logf(SRC, 0x193, LOG_INFO,
                "Standalone main ECU, going to shipping");
            *(uint8_t *)(s + 0x390) = 1;
            StateManager_ChangeState(self, ST_SHIPPING);
            rearm = 900000;
            break;
        }

        *(uint8_t *)(s + 0x390) = 0;

        if (lipo_current(lipo) < 1) {              /* not charging */
            common_logf(SRC, 0x19c, LOG_WARN, "LiPo is not charging");
            if (power_control_battery_up(pc)) {
                common_logf(SRC, 0x19e, LOG_INFO,
                    "Battery is ON but not charging LiPo");
                if (!buck_is_enabled(sw)) {        /* FUN_0011e420 */
                    common_logf(SRC, 0x1a0, LOG_INFO,
                        "Buck was off, enabling it");
                    buck_set_enable(sw, 1);
                } else {
                    common_logf(SRC, 0x1a3, LOG_INFO,
                        "Buck is enabled but let's see signals from charger IC");
                    common_logf(SRC, 0x1a4, LOG_INFO,
                        "PGOOD: %d MuxSwitch: %d VAC1: %d",
                        bq25672_read_pgood(lipo),
                        bq25672_read_muxswitch_tps2121(lipo),
                        bq25672_read_vac1_chrgstat0(lp));
                    if (!bq25672_read_pgood(lipo) ||
                        !bq25672_read_vac1_chrgstat0(lp)) {
                        common_logf(SRC, 0x1a7, LOG_INFO,
                            "Toggling BUCK because PGOOD and/or VAC1 are not OK");
                        buck_set_enable(sw, 0);
                        ts.tv_sec = 0; ts.tv_nsec = 100000000;
                        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
                        buck_set_enable(sw, 1);
                    }
                }
            }

            bq25672_refresh(lipo);                 /* FUN_00122810 */
            if (lipo_capacity(lipo) < 31) {
                if (monitor_primary_capacity(mon) == 0 ||
                    *(int *)(s + 0x394) == 10) {
                    common_logf(SRC, 0x1b1, LOG_INFO,
                        "Can't charge LiPo, going to shipping");
                    common_logf(SRC, 0x1b2, LOG_INFO,
                        "Primary RSOC: %d LiPo CheckTimes: %d",
                        monitor_primary_capacity(mon),
                        *(int *)(s + 0x394));
                    *(int *)(s + 0x394) = 0;
                    StateManager_ChangeState(self, ST_SHIPPING);
                    rearm = 15000;
                    timer_arm(((void **)self)[0x3e], rearm);
                    return;
                }
                *(int *)(s + 0x394) += 1;
            }
        } else {
            *(int *)(s + 0x394) = 0;
        }

        if (!lipo_charger_present(lipo))
            *(uint8_t *)(s + 0x120) = kStandbyEnter;
        rearm = 30000;
        break;
    }

    case kStandbyGoToSleep: {                      /* 2 */
        if (timer_running(((void **)self)[0x3f])) {  /* standby lp timer busy */
            rearm = 10000;
            break;
        }
        if (!power_control_battery_is_off(pc)) {
            common_logf(SRC, 0x1ca, LOG_INFO,
                "Battery should be off before we go to sleep");
            *(uint8_t *)(s + 0x120) = kStandbyEnter;
            rearm = 3000;
        } else {
            common_logf(SRC, 0x1d1, LOG_INFO,
                "Send standby request to battery, LPC and nRF devices");
            low_power_request_standby_can(lp);     /* FUN_00124860 */
            low_power_request_standby_nrf(lp);     /* FUN_001238c0 */
            *(uint8_t *)(s + 0x120) = kStandbyCheckCanQuiet;
            rearm = 1000;
        }
        break;
    }

    case kStandbyCheckCanQuiet: {                  /* 3 */
        common_logf(SRC, 0x1db, LOG_INFO,
            "Check that there is no traffic on the CAN bus");
        low_power_request_standby_can(lp);
        if (!low_power_can_quiet(lp, 500)) {       /* FUN_001239c0: 500ms window */
            common_logf(SRC, 0x1e2, LOG_ERR, "Still traffic on the CAN bus.");
            if (*(int *)(s + 0x398) < 3) {
                *(uint8_t *)(s + 0x120) = kStandbyGoToSleep;
                *(int *)(s + 0x398) += 1;
            } else {
                common_logf(SRC, 0x1e7, LOG_INFO, "Restarting the power sequence");
                *(uint8_t *)(s + 0x120) = kStandbyEnter;
                *(int *)(s + 0x398) = 0;
            }
        } else {
            *(uint8_t *)(s + 0x120) = kStandbySuspend;
            *(int *)(s + 0x398) = 0;
        }
        rearm = 1000;
        break;
    }

    case kStandbySuspend: {                        /* 4 */
        if (low_power_vac_present(lp)) {           /* FUN_001242f0 */
            common_logf(SRC, 499, LOG_INFO,
                "Voltage on VAC2, VAC1 and VBUS present. Stay awake.");
            rearm = 30000;
            break;
        }
        rearm = 10000;
        if (!timer_running(((void **)self)[0x3f])) {
            char r;
            common_logf(SRC, 0x1fa, LOG_INFO, "Go to sleep");
            rearm = 30000;
            low_power_prepare_suspend(lp, 1);      /* FUN_00124c40 */
            r = low_power_suspend_system(lp, 0x708, 1, 0);  /* FUN_00123c30 */
            if (r != 0) {
                if (r == 1) {
                    rearm = 1000;
                    timer_50ms_poll(self);
                } else {
                    rearm = 10000;
                    if (r == -1)
                        common_logf(SRC, 0x205, LOG_INFO, "SuspendSystem Error!");
                }
            }
            low_power_prepare_suspend(lp, 0);
        }
        *(uint8_t *)(s + 0x120) = kStandbyCheckCanQuiet;
        break;
    }

    default:
        /* defensive: drop the battery and restart at GoToSleep */
        switch_control_set_state(sw, ST_STANDBY);
        if (!power_control_battery_is_off(pc))
            battery_cmd0_off(pc);
        *(uint8_t *)(s + 0x120) = kStandbyGoToSleep;
        rearm = 10000;
        break;
    }

    timer_arm(((void **)self)[0x3e], rearm);
}

/* ================================================================== *
 *  Shared helpers used by the worker / Standby.
 * ================================================================== */

/* charger_in_standby                               OEM 0x00112fc0
 * Returns true while a charger is present in standby.  Decodes the charger
 * mode and drives the battery off / reset / charger-identify accordingly.
 * Returns false (and may drop to STANDBY) once charging finished (mode 3). */
bool charger_in_standby(PowerService *self)
{
    uint8_t *s   = (uint8_t *)self;
    uint8_t *pc  = s + 0x50;
    uint8_t *mon = s + 0x150;
    uint8_t mode;

    if (!power_control_charger_connected(pc))
        return false;

    mode = charger_decode_mode(mon);
    if (mode == 2) {                               /* generic fault */
        if (!monitor_battery_ins_det(mon) &&
            power_control_battery_is_off(pc)) {
            common_logf(SRC, 0x29d, LOG_INFO,
                "Battery is fully charged, powering off");
            battery_cmd0_off(pc);
        } else if (monitor_battery_ins_det(mon) &&
                   *(char *)(s + 0x391) == 0) {
            battery_cmd6_reset(pc);
            power_control_set_reset_grace(pc, DAT_0015d2d8);   /* FUN_00133570 */
            monitor_clear_charger_mode(mon);                   /* FUN_00128010 */
            *(uint8_t *)(s + 0x391) = 1;
        }
    } else if (mode == 0) {
        *(uint8_t *)(s + 0x121) = 3;
    } else if (mode == 1) {
        charger_cmd5_identify(pc);
    } else if (mode == 3) {
        if (!timer_running(((void **)self)[0x3e]) &&
            power_control_battery_is_off(pc)) {
            battery_cmd0_off(pc);
            *(uint8_t *)(s + 0x120) = kStandbyEnter;
            timer_arm(((void **)self)[0x3e], 3000);
            StateManager_ChangeState(self, ST_STANDBY);
        }
    }
    return mode != 3;
}

/* powerservice_refresh_clock_file                  OEM 0x00112870
 * system("touch /var/lib/systemd/timesync/clock ; sync") and log on failure. */
void powerservice_refresh_clock_file(void)
{
    int rc = system("touch /var/lib/systemd/timesync/clock ; sync");
    if (rc != 0)
        common_logf(SRC, 0x2b, LOG_ERR,
                    "Refreshing clock file timestamp failed with %s");
}

/* ================================================================== *
 *  Timer trampolines bound by the ctor.
 * ================================================================== */

/* 4000ms tick body                                 OEM ctor binds FUN_00112390 */
void timer_4000ms_charge_supervisor(PowerService **ctx)
{
    charge_supervisor_worker(*ctx);
}

/* 50ms poll trampoline (drives switch_control's fast path). */
void timer_50ms_poll_trampoline(PowerService **ctx)
{
    timer_50ms_poll(*ctx);
}
