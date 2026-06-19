#include "update_common.h"
#include <stdlib.h>
#include <time.h>

/* ===== module-local framework model (externs + structs) ===== */
/* SmpModemUpdateClient - the nRF9160 modem update client (an IUpdateClient).
 * OEM object: 8 words / 0x40 bytes. The ctor (0x11f2e0) stores the vtable at
 * [0], the OD/MQTT transport at [1], and zeroes [2..7] - that trailing region
 * holds the pthread_mutex_t locked around run_update (offset 0x10 == word 2).  */
typedef struct SmpModemUpdateClient {
    const void *vptr;       /* 0x00 [0] C++ vtable (DAT_0019cf28)           */
    void       *transport;  /* 0x08 [1] OD/MQTT transport (IUpdateClient)   */
    uint64_t    w2;         /* 0x10 [2] pthread_mutex_t storage (start)     */
    uint64_t    w3;         /* 0x18 [3]                                     */
    uint64_t    w4;         /* 0x20 [4]                                     */
    uint64_t    w5;         /* 0x28 [5]                                     */
    uint64_t    w6;         /* 0x30 [6]                                     */
    uint64_t    w7;         /* 0x38 [7]                                     */
} SmpModemUpdateClient;

/* nlohmann::json config blob, held by value in run_update. Concrete (not
 * opaque) because it is embedded by value; modelled as an opaque byte buffer
 * matching the OEM std::__cxx11 json object footprint (0x10 control word +
 * SSO/union). The real type is nlohmann::json. */
typedef struct json_obj { unsigned char data[16]; } json_obj;

/* struct timespec stand-in (avoids pulling <time.h> ABI into the model). */
typedef struct timespec_compat { long tv_sec; long tv_nsec; } timespec_compat;

/* ===== module-local framework model (everything beyond update_common.h) =====
 * SmpModemUpdateClient is an IUpdateClient over the VanMoof OD/MQTT transport;
 * field comments are OEM byte offsets from the decompiled object (program
 * "update", image base 0x100000). The C++ vtable, the std::async/future/thread
 * plumbing, the nlohmann::json config objects and the OD publish path are
 * modelled here as opaque externs - never rebuilt.
 */

/* IUpdateClient C++ vtable for this class (DAT_0019cf28). Slots observed:
 *   [0] 0x11e690 start_ppp   [1] 0x11e710 (dtor)   [2] 0x120320 run_update
 *   [3] 0x11e600 abort       [4] 0 (pure)                                    */
extern const void *SmpModemUpdateClient_vtable;

/* --- nlohmann::json modelled as a concrete value object (used by value in
 *     run_update for the OD config blobs). json_set_bool == operator[]=bool. */
void od_read(void *transport, const char *key, json_obj *out);            /* OD object read       */
void json_set_bool(json_obj *j, const char *field, bool value);           /* j[field] = value     */
void json_free(json_obj *j);

/* --- IUpdateClient transport OD/MQTT publish path (object at self->transport,
 *     i.e. param_1[1]). The two virtual ops actually called by run_update:    */
void od_clear_retained(void *transport, const char *key);                  /* vtable slot 0x30: publish empty payload, retain=true (clear retained msg) */
void od_publish_retained(void *transport, const char *key, const json_obj *j, bool retain); /* vtable slot 0x20: publish json, retained */

/* --- client lifetime mutex at offset 0x10 (pthread_mutex_t). --- */
void smp_client_lock(SmpModemUpdateClient *self);                          /* pthread_mutex_lock(self+0x10)   */
void smp_client_unlock(SmpModemUpdateClient *self);                        /* pthread_mutex_unlock(self+0x10) */

/* --- std::async / std::future model for system_execute_with_timeout.
 *     smp_async_run_system spawns a worker thread whose packaged_task runs
 *     system(cmd) (OEM thread-state vtable DAT_0019cf80, run target 0x11f040)
 *     and latches the int result into the shared future state.               */
typedef struct smp_future smp_future;
smp_future *smp_async_run_system(const char *cmd);
/* Wait up to timeout_ns (absolute deadline = now()+timeout_ns) on the future.
 * Returns nonzero if the worker finished (and stores its system() exit code in
 * *exit_code), 0 if the deadline elapsed first. (OEM __atomic_futex_unsigned
 * _M_futex_wait_until with deadline (now+600e9)/1e9 s + %1e9 ns.)             */
int  smp_future_wait_for(smp_future *fut, int64_t timeout_ns, int *exit_code);
void smp_future_destroy(smp_future *fut);

/* Read a numeric pid from a text file (OEM: std::ifstream + rdbuf into a
 * std::string, then strtol(s,0,10)). Returns 0 if the file was empty.         */
long smp_read_pid_file(const char *path);

/* nanosleep(&ts,&ts) looped while errno==EINTR (inline OEM pattern). --- */
void smp_nanosleep_eintr(struct timespec_compat *ts);
/* =========================================================== */

/*
 * smp_modem_update_client.c - VanMoof "update" OTA service
 *
 * Reconstruction of the nRF9160 modem firmware-update client. The modem runs a
 * Zephyr/MCUboot image that is flashed over the SMP / MCUmgr (mcumgr) serial
 * transport; on the i.MX8 host this client orchestrates the transfer by
 * (1) tearing down the live LTE/PPP link and the spi-mqtt-bridge that owns the
 * modem UART, (2) re-publishing a small set of retained MQTT/OD config keys so
 * the modem reboots into its stack-update path, (3) shelling out to the python
 * SMP transfer tool under a 600 s watchdog, then (4) bringing the bridge back.
 *
 * Behaviour-oriented translation of the decompiled AArch64 image (program
 * "update", image base 0x100000). Source path baked into the binary:
 *   devices/main/update/src/smp_modem_update_client.cpp
 *
 * Framework (std::string, nlohmann::json, the IUpdateClient OD/MQTT transport,
 * std::async / std::future, std::thread / mutex / condvar) is modelled via
 * update_common.h + the externs declared alongside this file - not rebuilt.
 * The systemctl/system() shell command strings, the OD config key names, the
 * /dev/ttymxc2 SMP transfer command line and the 600 s timeout are verbatim.
 */

/* ------------------------------------------------------------------------- */
/* Constant command lines and OD keys (verbatim from the image .rodata).     */
/* The big command lines live in .data globals assembled by the static       */
/* initialiser _INIT_5 (OEM 0x10d3a0); the pieces are reproduced here.       */
/* ------------------------------------------------------------------------- */

/* OD / retained-MQTT config keys (s_*_001*). */
#define OD_KEY_VERSION_INFO   "modem/nordic/version_info"     /* 0x171da8, len 0x19 */
#define OD_KEY_CONFIG_LTE     "modem/config/lte"              /* 0x171dc8, len 0x10 */
#define OD_KEY_NORDIC_UPDATE  "modem/nordic/update/config"    /* 0x171de0, len 0x1a */

/* JSON operator[] sub-keys used to set fields inside the config objects. */
#define OD_FIELD_LTE          "lte"     /* DAT_00171b68, len 3  - config_lte["lte"]=false */
#define OD_FIELD_INIT         "init"    /* "init"      , len 4  - nordic_update["init"]=true */

/* systemctl service control (passed straight to system()). */
#define CMD_STOP_PPP          "systemctl stop ppp@nrf9160"
#define CMD_START_PPP         "systemctl start ppp@nrf9160"
#define CMD_RESTART_BRIDGE    "systemctl restart spi-mqtt-bridge@modem"

/*
 * SMP transfer command, assembled by _INIT_5 (DAT_001a2100):
 *   "python3 " + "/usr/lib/python3.7/update_modem.py"  (split on " ", re-joined)
 *   + "/opt/devices_fw/mfw_nrf9160_1.3.1.zip"
 *   + " /dev/ttymxc2 115200"
 */
#define SMP_TRANSFER_CMD \
    "python3 /usr/lib/python3.7/update_modem.py " \
    "/opt/devices_fw/mfw_nrf9160_1.3.1.zip /dev/ttymxc2 115200"

/* Temp file the pid-find command redirects into (DAT_001a20e0). */
#define SMP_PID_FILE          "/tmp/smp_update_pid_control_file"

/* 600 s watchdog: 600000000000 ns, verbatim from FUN_0011f370. */
#define SMP_TIMEOUT_NS        600000000000LL

/* ------------------------------------------------------------------------- */
/* ctor - OEM 0x11f2e0. Constructs the SmpModemUpdateClient (an IUpdateClient)
 * over the supplied OD/MQTT transport, zeroes the seven trailing words (the
 * mutex + bookkeeping at 0x10..0x40) and stops PPP so the modem UART is free.
 * ------------------------------------------------------------------------- */
/* OEM 0x11f2e0 */
void smp_modem_update_client_ctor(SmpModemUpdateClient *self, void *transport) {
    int rc;

    self->vptr      = &SmpModemUpdateClient_vtable;   /* DAT_0019cf28 */
    self->transport = transport;                      /* [1] */
    self->w2 = 0; self->w3 = 0; self->w4 = 0;         /* [2..4] mutex storage */
    self->w5 = 0; self->w6 = 0; self->w7 = 0;         /* [5..7] */

    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x6a, LOG_INFO, "Stop PPP");
    rc = system(CMD_STOP_PPP);
    if (rc != 0) {
        common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                    0x6d, LOG_ERR, "Failed to Stop PPP: %d", rc);
    }
}

/* ------------------------------------------------------------------------- */
/* start_ppp - OEM 0x11e690 (IUpdateClient vtable slot 0). Resets the vtable
 * pointer (as the OEM does) and brings PPP back up on the nRF9160. Called when
 * the client is finished / torn down so the modem reconnects to LTE.
 * ------------------------------------------------------------------------- */
/* OEM 0x11e690 */
void smp_modem_update_client_start_ppp(SmpModemUpdateClient *self) {
    int rc;

    self->vptr = &SmpModemUpdateClient_vtable;        /* DAT_0019cf28 */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x72, LOG_INFO, "Start PPP");
    rc = system(CMD_START_PPP);
    if (rc != 0) {
        common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                    0x75, LOG_ERR, "Failed to Start PPP: %d", rc);
    }
}

/* ------------------------------------------------------------------------- */
/* abort - OEM 0x11e600 (IUpdateClient vtable slot 3). Best-effort recovery if
 * an update is cancelled mid-flight: just restart the modem's MQTT bridge so
 * the system returns to its normal "modem owned by the bridge" state.
 * ------------------------------------------------------------------------- */
/* OEM 0x11e600 */
void smp_modem_update_client_abort(SmpModemUpdateClient *self) {
    int rc;
    (void)self;

    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0xb2, LOG_INFO, "Abort update");
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0xb3, LOG_INFO, "restart SPI-MQTT-BRIDGE for modem");
    rc = system(CMD_RESTART_BRIDGE);
    if (rc != 0) {
        common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                    0xb6, LOG_ERR, "Failed to restart modem: %d", rc);
    }
}

/* ------------------------------------------------------------------------- */
/* system_execute_with_timeout - OEM 0x11f370.                               */
/*                                                                           */
/* Runs `cmd` via std::async(system, cmd) and waits on its future for up to   */
/* 600 s (SMP_TIMEOUT_NS). The future's std::packaged_task runs `system(cmd)` */
/* on a worker thread (OEM thread-state vtable DAT_0019cf80); the host blocks */
/* on the result's atomic-futex condition with an absolute deadline of        */
/* now()+600000000000 ns.                                                     */
/*                                                                           */
/* On normal completion: returns the shell exit status of `cmd` in bits 0..31 */
/* (the low word), top byte clear.                                            */
/*                                                                           */
/* On timeout: it must kill the still-running child. It builds                */
/*     ps | grep "<cmd>" | grep -v grep | awk '{print $1}' > <pidfile>        */
/* runs it, reads the pid back from <pidfile> (an ifstream slurped into a      */
/* std::string), strtol()s it, and if > 0 issues                              */
/*     kill -9 <pid>                                                          */
/* Returns 0xffffffff with bit 32 set (the "timed out" flag the caller tests  */
/* via `result & 0xff00000000`).                                              */
/*                                                                           */
/* The result is packed as: (timed_out ? 1<<32 : 0) | (low 32 = exit code).   */
/* ------------------------------------------------------------------------- */
/* OEM 0x11f370 */
uint64_t smp_modem_system_execute_with_timeout(const char *cmd, const char *grep_pattern) {
    smp_future *fut;
    int exit_code;
    int got;

    /* std::async(std::launch::async, system, cmd) - spawn the worker that
       calls system(cmd) and latches its int return into the shared state. */
    fut = smp_async_run_system(cmd);

    /* Block on the result for up to 600 s (absolute deadline now + 600e9 ns).
       got != 0  => the worker finished and `exit_code` is valid.
       got == 0  => the 600 s deadline elapsed first. */
    got = smp_future_wait_for(fut, SMP_TIMEOUT_NS, &exit_code);

    if (got) {
        /* Normal completion: forward the shell exit status, top byte clear. */
        smp_future_destroy(fut);
        return (uint64_t)(uint32_t)exit_code;  /* low word = exit status, top byte clear */
        /* NB: low word = exit_code; the caller treats (int)result != 0 as
           "command failed". Kept as a plain widen below for clarity. */
    }

    /* ---- timeout path ---- */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x46, LOG_WARN, "SystemExecuteWithTimeout : timeout");

    {
        char find_cmd[512];
        long pid;

        /* ps | grep "<pattern>" | grep -v grep | awk '{print $1}' > <pidfile> */
        snprintf(find_cmd, sizeof find_cmd,
                 "ps | grep \"%s\" | grep -v grep | awk '{print $1}' > %s",
                 grep_pattern, SMP_PID_FILE);
        if (system(find_cmd) != 0) {
            common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                        0x4e, LOG_ERR, "Failed to run pid_find_command %d",
                        system(find_cmd));
        }

        /* Slurp the pid back out of the temp file (OEM: ifstream + rdbuf). */
        pid = smp_read_pid_file(SMP_PID_FILE);
        if (pid == 0) {
            common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                        0x62, LOG_ERR, "Couldn't get the pid");
        } else if ((int)pid > 0) {
            char kill_cmd[64];
            int rc;
            snprintf(kill_cmd, sizeof kill_cmd, "kill -9 %d", (int)pid);
            rc = system(kill_cmd);
            if (rc != 0) {
                common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                            0x5b, LOG_ERR, "Failed to kill:%d, result:%d",
                            (int)pid, rc);
            }
        }
    }

    smp_future_destroy(fut);
    /* bit 32 set => "timed out"; low word = 0xffffffff (failure). */
    return (1ULL << 32) | 0xffffffffULL;
}

/* ------------------------------------------------------------------------- */
/* nanosleep-with-EINTR-retry - the inline pattern emitted at every wait in   */
/* run_update (nanosleep(&ts,&ts) looped while errno==EINTR).                 */
/* ------------------------------------------------------------------------- */
static void smp_sleep(time_t sec, long nsec) {
    struct timespec_compat ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    smp_nanosleep_eintr(&ts);
}

/* ------------------------------------------------------------------------- */
/* run_update - OEM 0x120320 (IUpdateClient vtable slot 2).                   */
/*                                                                           */
/* The full modem-update sequence, run under the client mutex (offset 0x10):  */
/*                                                                           */
/*  1. Clear the retained "modem/nordic/version_info" MQTT message (publish   */
/*     empty payload, retain=true) - vtable slot 0x30 on the transport.       */
/*  2. Disconnect LTE: read the "modem/config/lte" OD object, set its         */
/*     ["lte"]=false JSON field, publish it retained (slot 0x20). Settle      */
/*     100 ms.                                                                */
/*  3. Init the modem stack update: read "modem/nordic/update/config", set    */
/*     its ["init"]=true field, publish it retained. Settle 100 ms.           */
/*  4. Run the SMP transfer over /dev/ttymxc2 @115200 under the 600 s         */
/*     watchdog (system_execute_with_timeout). Returns 0 on success, -1 on a  */
/*     non-zero exit; a timeout additionally logs "<cmd> command timedout !". */
/*  5. Sleep 3 s, restart spi-mqtt-bridge@modem, sleep 1 s.                   */
/*                                                                           */
/* Returns 0 on success, 0xffffffff (-1) if the transfer command failed.      */
/* ------------------------------------------------------------------------- */
/* OEM 0x120320 */
uint32_t smp_modem_update_client_run_update(SmpModemUpdateClient *self) {
    void *transport = self->transport;            /* [1] */
    uint64_t res;
    uint32_t ret = 0;
    json_obj cfg;

    smp_client_lock(self);                        /* pthread_mutex_lock(self+0x10) */

    /* (1) Clear retained version_info: publish empty payload with retain=1. */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x7d, LOG_INFO,
                "Clear modem/nordic/version_info retained message");
    od_clear_retained(transport, OD_KEY_VERSION_INFO);   /* vtable slot 0x30 */

    /* (2) Disconnect LTE. */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x83, LOG_INFO, "Dissconnect LTE");
    od_read(transport, OD_KEY_CONFIG_LTE, &cfg);
    json_set_bool(&cfg, OD_FIELD_LTE, false);            /* cfg["lte"] = false */
    od_publish_retained(transport, OD_KEY_CONFIG_LTE, &cfg, true); /* slot 0x20 */
    json_free(&cfg);
    smp_sleep(0, 100000000);                             /* 100 ms */

    /* (3) Init modem stack update. */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x8d, LOG_INFO, "Init modem stack update");
    od_read(transport, OD_KEY_NORDIC_UPDATE, &cfg);
    json_set_bool(&cfg, OD_FIELD_INIT, true);            /* cfg["init"] = true */
    od_publish_retained(transport, OD_KEY_NORDIC_UPDATE, &cfg, true);
    json_free(&cfg);
    smp_sleep(0, 100000000);                             /* 100 ms */

    /* (4) Run the SMP transfer under the 600 s watchdog. */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0x96, LOG_INFO, "Start transfering ober SMP server");
    res = smp_modem_system_execute_with_timeout(SMP_TRANSFER_CMD, SMP_TRANSFER_CMD);

    if ((int)res != 0) {
        common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                    0x9a, LOG_ERR, "Failed to run update command: %d",
                    (uint32_t)res);
        ret = 0xffffffff;
    }
    if ((res & 0xff00000000ULL) != 0) {
        common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                    0xa2, LOG_ERR, "%s command timedout !", SMP_TRANSFER_CMD);
    }

    /* (5) Settle, then bring the bridge back. */
    smp_sleep(3, 0);                                     /* 3 s */
    common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                0xa7, LOG_INFO, "restart SPI-MQTT-BRIDGE for modem");
    {
        int rc = system(CMD_RESTART_BRIDGE);
        if (rc != 0) {
            common_logf("devices/main/update/src/smp_modem_update_client.cpp",
                        0xaa, LOG_ERR, "Failed to restart modem: %d", rc);
        }
    }
    smp_sleep(1, 0);                                     /* 1 s */

    smp_client_unlock(self);                             /* pthread_mutex_unlock */
    return ret;
}