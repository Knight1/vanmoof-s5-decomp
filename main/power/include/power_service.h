/*
 * devices/main/power/include/power_service.h  (reconstructed)
 *
 * Public surface of the 'power' service top level plus the framework externs
 * this TU relies on.  These declarations model STL/std::function/vm plumbing
 * through behaviour-level interfaces; the real symbols are provided by the
 * service framework and the sibling power-module headers.
 */
#ifndef POWER_SERVICE_H
#define POWER_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>

#include "power_common.h"   /* PowerService, enums, common_logf, PS_* macros */

/* The StateManager base is opaque here (PowerService embeds it at +0x118). */
typedef struct StateManager StateManager;
/* Generic callback type - handlers/workers are cast to this at the call site
 * (ISO C disallows implicit function-pointer <-> void* conversion). */
typedef void (*ps_cb)(void);
void ps_mutex_lock(void *m);
void ps_mutex_unlock(void *m);

/* ---- service entry / lifecycle -------------------------------------- */
int  main(int argc, char **argv);
void PowerService_ctor(PowerService *self, void *ipc, void *clock,
                       void *timerfactory, void *config);
void PowerService_dtor(PowerService *self);
void PowerService_Run(PowerService *self);
void PowerService_Stop(PowerService *self);
void PowerService_TurnOn(PowerService *self);

/* ---- vtable state handlers (slots +0x10..+0x40) --------------------- */
void PowerService_OnStateChanged(PowerService *self, int state);
void PowerService_OnShipping(PowerService *self);
void PowerService_OnStandby(PowerService *self);
void PowerService_OnOperational(PowerService *self);
void PowerService_OnCharging(PowerService *self);
void PowerService_OnUpdating(PowerService *self);
void PowerService_OnAlarm(PowerService *self);
void PowerService_OnMaintenance(PowerService *self);

/* ---- MQTT handlers --------------------------------------------------- */
void on_mqtt_state_set(PowerService **ctx, void *topic, void *meta, void *json);
void on_mqtt_state_extend_timeout(PowerService **ctx, void *topic, void *meta,
                                  void *json);
void on_mqtt_low_power_extend(PowerService **ctx, void *topic, void *meta,
                              void *json);
void on_mqtt_modem_time(PowerService **ctx, void *topic, void *meta, void *json,
                        void *meta2, const char *err);
void on_mqtt_battery_reset(PowerService **ctx, void *topic, void *meta,
                           void *json);

/* ---- worker / helpers ------------------------------------------------ */
void charge_supervisor_worker(PowerService *self);
bool charger_in_standby(PowerService *self);
void powerservice_refresh_clock_file(void);
void timer_4000ms_charge_supervisor(PowerService **ctx);
void timer_50ms_poll_trampoline(PowerService **ctx);

/* ===================================================================== *
 *  Framework externs (modelling STL/std::function/vm + sibling modules).
 *  Real implementations live in the service runtime / module objects.
 * ===================================================================== */

/* ServiceEnv (devices/main/power/src/main.cpp env) -- FUN_001418* / 1402*0 */
typedef struct ServiceEnv ServiceEnv;
void  service_env_init(ServiceEnv *env, int argc, char **argv,
                       const char *name, int flags);
void *service_env_ipc(ServiceEnv *env);          /* FUN_001402e0 */
void *service_env_clock(ServiceEnv *env);        /* FUN_001402f0 */
void *service_env_timerfactory(ServiceEnv *env); /* FUN_00140300 */
void *service_env_config(ServiceEnv *env);       /* FUN_00140310 */
void  service_env_run(ServiceEnv *env);          /* FUN_001402b0 */
void  service_env_destroy(ServiceEnv *env);      /* FUN_00140080 */
struct ServiceEnv { uint8_t _raw[48]; };

/* allocator / runtime shims */
void *operator_new(size_t n);                    /* FUN_00109810 */
void  operator_delete(void *p, size_t n);        /* FUN_00109820 */
uint64_t clock_now_ns(void);                     /* FUN_00109080 */

/* od / message-bus subscription + tiny JSON decoders */
void od_subscribe(void *bus, const char *topic, ps_cb handler, void *ctx);
void od_unsubscribe(void *bus, const char *topic);
void od_parse_int(void *json, int *out, int dflt);
void od_parse_int_signed(void *json, int *out);
void od_parse_uint(void *json, int *out);
bool od_is_number(void *json);
bool od_obj_has(void *json, const char *key);
bool od_obj_get_bool(void *json, const char *key);
void od_obj_get_int(void *json, const char *key, int *out);
void od_obj_get_string(void *json, const char *key, char *out, size_t n);

/* periodic timers (timerfactory vtable +0x10 periodic / +0x18 fast) */
void *make_periodic_timer(void *factory, uint32_t ms, ps_cb handler, void *ctx);
void *make_periodic_timer_fast(void *factory, uint32_t ms, ps_cb handler,
                               void *ctx);
void  timer_arm(void *timer, uint64_t ms);       /* vtable +0x20 */
void  timer_stop(void *timer);                   /* vtable +0x10 */
bool  timer_running(void *timer);                /* vtable +0x28 */
void  timer_release(void *timer);                /* vtable +0x08 */

/* StateManager (base of PowerService) -- state_manager.h companions */
void StateManager_ctor(PowerService *self, void *timerfactory, void *config);
void StateManager_dtor(PowerService *self);
void StateManager_ChangeState(PowerService *self, int state);
int  StateManager_CurrentState(PowerService *self);          /* FUN_0011ac70 */
int  StateManager_RequestedState(PowerService *self);        /* FUN_0011ac80 (+0x44) */
bool StateManager_InTransition(PowerService *self);          /* FUN_0011ac90 */
void StateManager_OnStateRequest(PowerService *self, int state, int src);
void StateManager_SetExtendTimeout(void *sm, int secs);
void StateManager_StateName(PowerService *self, char *out);
void StateManager_RequestedStateName(PowerService *self, char *out);
void state_manager_bind_on_state(void *smbase, ps_cb handler, void *ctx);
void state_manager_subscribe_requests(void *config, ps_cb handler, void *ctx);
void state_manager_set_clock(void *smbase, const char *timestr); /* FUN_0011a080 */
void state_client_start(PowerService *self);                  /* FUN_0011a7e0 */
void state_client_join(void *pc);                            /* FUN_001332d0 */
void *PS_state_vtable_addr(PowerService *self);
void *state_manager_vtable_addr(PowerService *self);
#define PS_state_vtable(self)      (*(void **)((uint8_t *)(self) + 0x00))
#define state_manager_vtable(self) (*(void **)((uint8_t *)(self) + 0x50))

/* PowerControl (+0x50) -- power_control.h companions */
void PowerControl_ctor(void *pc, int retries, int interval);
void PowerControl_dtor(void *pc);
bool power_control_charger_connected(void *pc);   /* FUN_00133430 */
bool power_control_charger_off(void *pc);         /* FUN_00133470 */
bool power_control_battery_up(void *pc);          /* FUN_001333f0 */
bool power_control_battery_is_off(void *pc);      /* FUN_00133490 */
bool get_is_primary_inserted(void *pc);
void power_control_set_reset_grace(void *pc, uint32_t v);   /* FUN_00133570 */
bool power_control_buck_needed(void *pc);   /* FUN_001332f0: charger present & within retry window */
bool power_control_retry_pending(void *pc); /* FUN_00133410: counter +0x30 < threshold +0x4a       */

/* battery power-on / charging supervisor -- battery_power_on.c (OEM
 * power_service.cpp virtuals; were MISSED by Ghidra auto-analysis). */
void battery_power_supervise(PowerService *self);   /* FUN_00112560 buck/fault watchdog   */
bool battery_charging_poll(PowerService *self);     /* FUN_00112fc0 charger-mode evaluator */
void battery_power_on_recovery(PowerService *self); /* FUN_00113130 "Battery error!"       */
void battery_power_on_sequence(PowerService *self); /* FUN_00113280 power-on supervisor     */
void battery_enter_charging(PowerService *self);    /* FUN_00113580 enter-charging          */
void buck_drive_on(void *sw);                       /* FUN_00142900 buck GPIO toggle        */
void eshifter_request_calibration(void *calib);     /* FUN_00120680 eshifter FSM (prose)    */

/* Monitor (+0x150) -- monitor companions */
void Monitor_ctor(void *mon, void *ipc, void *clock, void *tf, void *lipo);
void Monitor_dtor(void *mon);
int  monitor_primary_capacity(void *mon);         /* FUN_00128130 */
bool monitor_battery_ins_det(void *mon);          /* FUN_00128170/0x1281b0 */
bool monitor_battery_ins_det_fail(void *mon);     /* FUN_00128190 */
bool monitor_standalone(void *mon);               /* FUN_00127ec0 */
uint8_t charger_decode_mode(void *mon);
void monitor_clear_charger_mode(void *mon);       /* FUN_00128010 */

/* LiPo / BQ25672 gauge (+0x128) -- lipo_control.h companions */
void lipo_ctor(void *lipo);
void lipo_dtor(void *lipo);
void lipo_bq25672_write_regs_standby(void *lipo, void *unused, int v);
bool lipo_charger_present(void *lipo);            /* FUN_00121ce0 */
bool lipo_is_dead(void *lipo);                    /* FUN_00121cc0 */
int  lipo_capacity(void *lipo);                   /* FUN_001213c0 */
int  lipo_current(void *lipo);                    /* FUN_001219c0 */
void lipo_charge_complete(void *lipo);            /* FUN_00122440 */
void bq25672_refresh(void *lipo);                 /* FUN_00122810 */
bool bq25672_read_pgood(void *lipo);
bool bq25672_read_muxswitch_tps2121(void *lipo);
bool bq25672_read_vac1_chrgstat0(void *lp);

/* switch_control (+0x208) -- switch_control.h companions */
void switch_control_ctor(void *sw, void *ipc, void *iface, void *clock);
void switch_control_dtor(void *sw);
void switch_control_set_state(void *sw, int state);        /* FUN_0011e430 */
void switch_control_apply_standby(void *sw, int state);
void switch_control_power_on(void *fast_timer);            /* +0x18 trampoline */
bool switch_control_power_on_seq_running(void *fast_timer);/* vtable +0x28 */
void buck_set_enable(void *sw, int on);
bool buck_is_enabled(void *sw);                            /* FUN_0011e420 */

/* low_power (+0x3a0) -- low_power.h companions */
void low_power_ctor(void *lp, void *config, void *clock, void *tf);
void low_power_dtor(void *lp);
void low_power_publish_low_power(void *lp);                /* FUN_00125030 */
void low_power_request_can_low_power(void *lp);            /* FUN_00123940 */
void low_power_reset_fuel_gauge(void *lp);                 /* FUN_00123e20 */
void low_power_request_standby_can(void *lp);             /* FUN_00124860 */
void low_power_request_standby_nrf(void *lp);             /* FUN_001238c0 */
bool low_power_can_quiet(void *lp, int window_ms);         /* FUN_001239c0 */
bool low_power_vac_present(void *lp);                      /* FUN_001242f0 */
void low_power_prepare_suspend(void *lp, int on);          /* FUN_00124c40 */
char low_power_suspend_system(void *lp, int wakeups, int a, int b); /* FUN_00123c30 */

/* battery / charger commands -- battery_decode.h companions */
void battery_cmd0_off(void *pc);
void battery_cmd1_on(void *pc);
void battery_cmd6_reset(void *pc);
void charger_cmd5_identify(void *pc);

/* misc charge bookkeeping */
void charge_counters_reset(void *counters);               /* FUN_0011fed0 */
void rtc_set_time(void *lp_handle, const char *timestr);  /* +0x408 vtable +0x28 */

/* timer_50ms_poll body lives next to switch_control (poll one tick). */
void timer_50ms_poll(PowerService *self);

/* C++ rodata blobs referenced verbatim by the helpers above. */
extern uint8_t  DAT_00184a88[];   /* PowerService vtable */
extern uint8_t  DAT_00184b20[];   /* StateManager vtable */
extern uint32_t DAT_0015d2d8;     /* battery reset grace constant */

#endif /* POWER_SERVICE_H */
