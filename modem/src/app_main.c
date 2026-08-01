/*
 * app_main.c -- VanMoof S5 modem (nRF9160) -- MQTT-FTP relay + watchdog heartbeat + modem DFU + main/init.
 *
 * Reconstructed (behaviour-oriented C) from the OEM image
 *   modem.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (Nordic nRF9160, Cortex-M33, Zephyr RTOS + nRF Connect SDK; MCUboot, base 0x0).
 * Only VanMoof application code is reconstructed; Zephyr (k_*/net_*/socket), the
 * nRF Connect SDK (lte_lc/nrf_modem/location/sms/dfu_target) and mbedTLS/newlib
 * are vendor (extern). OEM addresses are in each function header.
 */
#include "modem.h"


/* ===== wdt_heartbeat_cb  (OEM 0x0001378) [confidence: medium] =====
 * WDT interrupt service routine / timeout callback installed by wdt_heartbeat_cb_setup. Fires when the WDT expires without being fed. Accesses nRF9160 NVIC ISPR at 0x50003800 to clear the pending interrupt bit, issues DSB+ISB barriers, reads/reloads the WDT channel counter from 0x50003000 (nRF SPU mapped peripheral), then calls FUN_00007a98 to perform the system reset or WDT feed action. LAB_0000138 */
/* OEM addr 0x0001378 — wdt_heartbeat_cb: WDT ISR; LAB_00001388 is dispatch point */
int wdt_heartbeat_cb(uint32_t channel, undefined4 param_2,
                     undefined4 param_3, undefined4 param_4)
{
    if ((int8_t)channel >= 0) {
        /* clear NVIC ISPR for this IRQ */
        *(uint32_t *)(DAT_000013cc + (((uint8_t)channel >> 5) + 0x20) * 4) =
            1u << (channel & 0x1f);
        __DSB();
        __ISB();
    }
    uint32_t cfg   = DAT_000013d0;
    int      index = channel * 4;
    uint32_t cur   = *(uint32_t *)(index + 0x50003800);
    if (channel == 0x31 ||
        ((int32_t)cur < 0 && (cur = (cur & 3u) - 2u, cur < 2u))) {
        *(uint32_t *)(index + 0x50003800) = cfg;
        cur = cfg;
    }
    FUN_00007a98(channel, 1, cur, index + 0x50003000, param_4);
    return 0;
}


/* ===== reboot_handler  (OEM 0x00013408) [confidence: medium] =====
 * MQTT reboot topic handler (topic code 0x10c1). Receives a CBOR-encoded reboot request containing 'req_update' and 'warm' fields. If 'req_update' is present it triggers a firmware-update reboot sequence via nrf_modem, calls FUN_00030fe0(0x8000, 0) to set the WDT reload for reboot, then FUN_00037614(0) to reboot. Copies LTE config from DAT_000134a8 before and after the CBOR parse for state restorati */
/* OEM addr 0x00013408 — reboot_handler: MQTT reboot command, triggers sys_reboot */
void reboot_handler(void)
{
    int rc;
    uint32_t lte_cfg[2];
    lte_cfg[0] = *DAT_000134a8;
    lte_cfg[1] = *(uint16_t *)(DAT_000134a8 + 4);
    /* CBOR decode: look for 'req_update' and 'warm' fields */
    uint8_t msg[32];
    FUN_0002d154(msg, 0, 0);
    if (FUN_0003bd18(msg, 0, /*key_buf*/0, /*val_buf*/0) == 0) {
        if (FUN_0003be6c(/*val_buf*/, DAT_000134ac, /*out*/) == 0) {
            /* 'req_update' field present: relay 6 bytes of LTE config */
            FUN_0002d1a0(/*val_buf*/, <e_cfg, &(uint32_t){6}, 0);
        }
        if (FUN_0003be6c(/*val_buf*/, DAT_000134b0, /*out*/) == 0 &&
            /*decoded warm flag*/ != 0) {
            FUN_000341da(DAT_000134b4);   /* nrf_modem_dfu_schedule or similar */
            FUN_00030fe0(0x8000, 0);      /* wdt_setup(chan, timeout_ms=1000) */
            FUN_00037614(0);              /* sys_reboot(SYS_REBOOT_COLD=0) */
        }
    }
    /* update LTE config state */
    FUN_0003e710(DAT_000134b8, <e_cfg);
    FUN_000341da(DAT_000134bc, <e_cfg);
    FUN_0003e710(DAT_000134b8, <e_cfg);
    FUN_00017984(/*rc*/ != 0); /* fatal error if still running */
}


/* ===== modem_main  (OEM 0x0013F3C) [confidence: high] =====
 * Main LTE connectivity and MQTT setup loop. Single caller: NMI_Handler. Initialises modem, brings up LTE, configures MQTT broker connections, installs subscription handlers, starts heartbeat and WDT, then enters the reconnect/publish loop. All VanMoof subsystem initialisation (heartbeat, WDT, FTP, cert/config publishing) branches from here. */
/* OEM addr 0x0013F3C — VanMoof main LTE+MQTT setup loop; one caller: NMI_Handler */
void modem_main(void)
{
    FUN_00037634();                          /* nrf_modem_lib_init or LTE-M init */
    FUN_00037662();                          /* lte_lc_connect */
    FUN_00016bf8();                          /* nrf_heartbeat_init (WDT + timer) */
    FUN_00016da4();                          /* wdt_heartbeat_cb_setup */
    FUN_0001561c();                          /* connectivity check */
    FUN_00028f7c(DAT_00014130);
    FUN_00028ea4(DAT_00014134);
    FUN_00014898();                          /* WDT/timer initialisation */
    FUN_000295d8(DAT_00014138, 0);           /* MQTT client setup */
    FUN_0003d902(DAT_00014140, DAT_00014144);
    FUN_0002e854(DAT_00014148);              /* broker connect */
    FUN_0002e854(DAT_00014158);              /* broker connect (alt) */
    FUN_0003b17e(DAT_00014160);
    FUN_0002727c(1);
    FUN_00027630(1);                         /* socket connect (IPv4/IPv6) */
    FUN_00015138();                          /* MQTT subscribe setup (AT / location) */
    FUN_00027d88(DAT_00014150);
    FUN_00013e14();                          /* CBOR + MQTT publish (generic state) */
    FUN_00013624();                          /* CBOR + MQTT publish (FTP state) */
    FUN_0001385c();                          /* CBOR + MQTT publish (certs/config) */
}


/* ===== wdt_timer_init  (OEM 0x0014898) [confidence: medium] =====
 * WDT and MQTT timer initialisation called from modem_main. Sets up the WDT device struct fields (DAT_00014914 + offsets 4 and 8), calls FUN_000206a0 (WDT driver init), configures MQTT keepalive timer via FUN_0003d902, opens a WDT channel via FUN_0003d2cc, then sets the WDT reload value by spinning on the 0x22c flag in the modem IPC object and writing DAT_00014938 to offset 0x16c. */
/* OEM addr 0x0014898 — wdt_timer_init: WDT device + MQTT keepalive timer setup */
void wdt_timer_init(void)
{
    int dev, rc;
    dev = DAT_00014914;
    *(uint32_t *)(DAT_00014914 + 4) = DAT_00014918;
    *(uint32_t *)(dev + 8)          = DAT_0001491c;
    FUN_000206a0();                              /* wdt_driver_init */
    FUN_0003d902(DAT_00014924, DAT_00014920);    /* k_timer_init(keepalive_timer, expiry_fn) */
    rc = FUN_0003d2cc(DAT_00014928);             /* k_timer_start or wdt_setup check */
    if (rc == 0 && (*DAT_0001492c & 6) != 0) {
        uint32_t args[2] = {2, DAT_00014930};
        FUN_00033e14(DAT_0001492c, 0x1080, args); /* LOG_WRN */
    }
    dev = FUN_0001eb80(0);                       /* nrf_modem_get_ipc_obj */
    if (dev == 0) {
        if (*DAT_0001492c & 7) {
            uint32_t args[2] = {2, DAT_00014934};
            FUN_00033e14(DAT_0001492c, &DAT_00001040, args); /* LOG_ERR */
        }
    } else {
        while (!((*(uint8_t *)(dev + 0x22c)) & 4)) {
            FUN_00030ed0(); /* k_yield */
        }
        *(uint32_t *)(dev + 0x16c) = DAT_00014938; /* set WDT reload value */
    }
}


/* ===== ftp_command_handler  (OEM 0x00158BC) [confidence: high] =====
 * MQTT ftp_command topic dispatch handler (topic code 0x1000). Registered in the 12-byte MQTT topic handler table at flash 0x73670. Receives an incoming CBOR-encoded FTP command payload from the iMX8 main CPU via MQTT and dispatches to one of five operations using a switch on the decoded command code: 0=start transfer (validate key, open flash partition), 1=cancel, 2=write chunk (erase+program flash */
/* OEM addr 0x00158BC — ftp_command_handler: MQTT ftp_command CBOR dispatcher */
void ftp_command_handler(undefined4 param_1, undefined4 param_2,
                         undefined4 param_3, undefined4 param_4,
                         undefined4 param_5)
{
    /* decode CBOR envelope: expects {cmd: int, ...} map */
    uint8_t cbor_enc[16], cbor_buf[152], msg[36], key_buf[16];
    int  cmd_code, result;
    FUN_0002d18c(cbor_enc, DAT_00015b00, 0x96);   /* cbor_init(out, buf, 0x96) */
    FUN_0003b872(local_b0, cbor_enc, 0);           /* cbor_encoder_create_map */
    FUN_0002d154(msg, param_4, param_5);           /* decode incoming CBOR msg */
    if (FUN_0003bd18(msg, 0, key_buf, cbor_enc) != 0 ||
        FUN_00033e6e(cbor_enc, &cmd_code) != 0) {
        result = 6;  /* parse error */
        goto reply;
    }
    switch (cmd_code) {
    case 0: /* start */
        /* validate transfer key, open flash partition */
        if (*DAT_00015b1c == 0) {
            /* parse key + size from CBOR, call FUN_000341da / FUN_0003e710 */
            FUN_000341da(DAT_00015b34, DAT_00015b2c, DAT_00015b30, /*len*/0);
            *DAT_00015b1c = DAT_00015b38; /* store flash handle */
            result = 0;
        } else { result = 2; /* already open */ }
        break;
    case 1: /* cancel */
        if (*DAT_00015db8 && *(code **)(*DAT_00015db8 + 0x14))
            (*(code **)(*DAT_00015db8 + 0x14))(2);
        *DAT_00015db8 = 0;
        result = 0;
        break;
    case 2: /* write chunk */
        /* erase if aligned, then program flash page */
        if (*DAT_00015db8 == 0) { result = 9; break; }
        /* parse offset + data, FUN_0003e710 write, FUN_000341da erase if needed */
        result = 0;
        break;
    case 3: /* end */
        /* verify CRC32 over transfer */
        result = 0;
        break;
    case 4: /* checksum */
        /* compute CRC32 of flash region, reply with value */
        result = 0;
        break;
    default:
        result = 7;
    }
reply:
    /* CBOR-encode {cmd, status} and mqtt_publish reply */
    FUN_00033e92(local_b0, DAT_00015b0c);           /* encode "cmd" key */
    FUN_0003b88c(local_b0, 0, cmd_code, 0);         /* encode cmd value */
    FUN_00033e92(local_b0, DAT_00015b10);           /* encode "status" key */
    FUN_0003b88c(local_b0, 0, result, 0);           /* encode status value */
    FUN_0003b972(local_b0, cbor_enc);               /* close map */
    FUN_00015e74(param_3, &DAT_00001001, DAT_00015de4,
                 *(uint32_t *)(local_b0[0] + 4));   /* mqtt_publish reply */
}


/* ===== nrf_heartbeat_init  (OEM 0x00016BF8) [confidence: medium] =====
 * Heartbeat subsystem initialisation: opens the WDT device via the Zephyr WDT driver vtable (5000 ms window), then starts the periodic k_timer that drives heartbeat MQTT publishes. Called from modem_main during LTE/MQTT setup. */
/* OEM addr 0x00016BF8 — nrf_heartbeat_init: WDT open + k_timer start */
void nrf_heartbeat_init(void)
{
    int rc;
    int channel;
    rc = FUN_0003d2cc(DAT_00016c5c);          /* k_timer_stop / pre-check */
    if (rc == 0 && (*DAT_00016c60 & 7) != 0) {
        uint32_t args[2] = {2, DAT_00016c64};
        FUN_000180ac(DAT_00016c60, &DAT_00001040, args, 0); /* LOG_WRN */
    }
    channel = FUN_0001c020(DAT_00016c5c);     /* wdt_setup(dev, flags=2, timeout_ms=5000) */
    if (channel != 0 && (*DAT_00016c60 & 7) != 0) {
        uint32_t args[2] = {3, DAT_00016c68};
        FUN_000180ac(DAT_00016c60, &DAT_00001840, args, 0); /* LOG_INF */
    }
}


/* ===== heartbeat_publish  (OEM 0x00016CC4) [confidence: high] =====
 * Periodic heartbeat MQTT publisher. No static callers — invoked by the k_timer expiry callback or from modem_main as the periodic publish. Acquires a lock, CBOR-encodes battery/state telemetry (reads a 64-bit counter from DAT_00016d64), calls FUN_00015e74 to MQTT-publish on topic code 0xd1, clears the delta bits on success, then reschedules via FUN_00016cb8. */
/* OEM addr 0x00016CC4 — heartbeat_publish: CBOR encode + MQTT publish nrf_heartbeat */
void heartbeat_publish(void)
{
    uint32_t *state = DAT_00016d64;
    int local_e0[4];
    uint8_t cbor_buf[152];
    uint8_t cbor_enc[16];

    FUN_00016c90(0);                                /* WDT feed (channel 0) */
    FUN_0003e3a6(cbor_buf, 0, 0x92);               /* memset */
    FUN_0002d18c(cbor_enc, cbor_buf, 0x96);         /* cbor_init */
    FUN_0003b872(local_e0, cbor_enc, 0);            /* cbor_encoder_create_map */
    FUN_000301f0(DAT_00016d68, 0, 0xffffffff, 0xffffffff); /* mutex_lock */
    FUN_0003b87c(local_e0, 0, state[0], state[1]); /* cbor_encode_uint64 (state) */
    int rc = FUN_00015e74(0x81, &DAT_000010d1, cbor_buf,
                          *(uint32_t *)(local_e0[0] + 4)); /* mqtt_publish topic=0xd1 */
    if (rc == 0) {
        state[0] &= ~state[2];
        state[1] &= ~state[3];
        state[2] = 0;
        state[3] = 0;
    } else if ((*DAT_00016d6c & 7) != 0) {
        uint32_t args[2] = {2, DAT_00016d70}; /* "Failed publishing heartbeat event" */
        FUN_000180ac(DAT_00016d6c, &DAT_00001040, args, 0);
    }
    FUN_00016cb8();  /* k_timer_start: reschedule periodic publish */
}


/* ===== wdt_heartbeat_cb_setup  (OEM 0x00016DA4) [confidence: medium] =====
 * Registers the WDT timeout callback with the nRF9160 WDT driver. Calls wdt_setup on the WDT device, then wdt_install_timeout to install the callback function at 0x1388 (wdt_heartbeat_cb) with a 32768-tick window. Stores the returned channel ID in DAT_00016c8c. Called from modem_main. */
/* OEM addr 0x00016DA4 — wdt_heartbeat_cb_setup: install WDT timeout callback */
void wdt_heartbeat_cb_setup(void)
{
    /* wdt_setup(device=DAT_00016dc8, flags=WDT_OPT_PAUSE_HALTED_BY_DBG) */
    FUN_0003150c(DAT_00016dc8);
    /* wdt_install_timeout(device, callback=wdt_heartbeat_cb@0x1388,
     *                     cfg=DAT_00016dcc, flags=0) -> channel_id */
    uint32_t channel = FUN_0001c10c(&LAB_00001388, DAT_00016dcc, 0);
    uint8_t *state = DAT_00016c8c;
    *state = 1;                         /* mark WDT active */
    *(uint32_t *)(state + 1) = channel; /* store channel handle */
}


/* ===== NMI_Handler  (OEM 0x002FAF6) [confidence: high] =====
 * Zephyr main application thread entry. The nRF9160 MCUboot image stores this in the NMI vector slot (offset 0x208 of the application vector table at 0x200). Zephyr repurposes NMI as the k_thread entry for the privileged application thread. Calls SYS_INIT level runners, nrf_modem_lib init, the VanMoof app init array runner, thread creation loop, then drops into the LTE/MQTT connection loop. */
/* OEM addr 0x002FAF6 — Zephyr main application thread (NMI vector slot) */
void NMI_Handler(undefined4 param_1, undefined4 param_2, undefined1 param_3)
{
    *DAT_0002fb24 = param_3;
    FUN_0002f918(2);       /* Zephyr SYS_INIT level 2 (POST_KERNEL) runner */
    FUN_0003170c();        /* nrf_modem_lib_init wrapper */
    FUN_00035fba();        /* VanMoof app init array runner */
    FUN_0002f918(3);       /* Zephyr SYS_INIT level 3 (APPLICATION) runner */
    FUN_0002ff94();        /* K_THREAD_DEFINE thread creation loop */
    FUN_00013f3c();        /* main LTE + MQTT connection/reconnect loop */
    *(byte *)(DAT_0002fb28 + 0xc) = *(byte *)(DAT_0002fb28 + 0xc) & 0xfe;
}


/* ===== app_init_array_runner  (OEM 0x00035FBA) [confidence: high] =====
 * VanMoof application init array runner. Called between SYS_INIT level 2 and level 3 from NMI_Handler. Iterates a flash-resident table of function pointers (registered with __attribute__((constructor)) or equivalent linker magic) and calls each init function in order. */
/* OEM addr 0x00035FBA — VanMoof app init-function table runner */
void app_init_array_runner(void)
{
    FUN_0001ada4();            /* reverse-order pre-init (decrements through array) */
    for (void (**p)(void) = DAT_0001ad9c; p < DAT_0001ada0; p++) {
        (*p)();
    }
}
