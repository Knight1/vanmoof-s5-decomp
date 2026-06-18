/*
 * monitor.h — battery/charger telemetry monitor (devices/main/power, monitor.cpp)
 *
 * OEM: /usr/bin/power. The Monitor owns the OD-signal registrations that decode
 * the primary-battery CAN payloads and the device/charger subscriptions that
 * track the external charger. It is the PowerService sub-object at this+0x150.
 *
 * Behaviour-oriented reconstruction: the std::vector<OdRegistration>, the
 * std::function callbacks and the std::string topics are modelled through the
 * shared OD/subscription interface (od_register_signal / od_subscribe). The bit
 * extraction in the two flag decoders is reproduced verbatim; runtime-named
 * boolean topics carry documented placeholder names (the real names are built
 * from static std::string globals that are not statically recoverable).
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdint.h>
#include <stdbool.h>

/* ---- the Monitor object (OEM offsets on `this`) ----------------------- */

/*
 * Field map recovered from monitor.cpp (battery_status_decode /
 * charger_decode_mode / monitor_charger_connected_publish). Offsets are on the
 * Monitor `this` pointer (PS_monitor(ps) == ps+0x150).
 */
struct Monitor {
    void    *vtable;          /* this+0x00  &PTR_FUN_00184440                 */
    void    *od;              /* this+0x08  OD handle (signal registration)   */
    void    *ipc;             /* this+0x10  publish / IPC client (vtable+0x20) */
    /* this+0x18..0x30  std::vector<OdRegistration> {begin,end,cap}            */
    void    *charger_client;  /* this+0x30  device/charger subscription source */
    void    *power_control;   /* this+0x50  charger-connected gate (vtable)    */
    void    *charger_in;      /* this+0x58  charger-present gate (vtable+0x18)  */
    /* cached telemetry (all at fixed offsets, read by charger_decode_mode):   */
    uint16_t soc;             /* this+0x80  raw state-of-charge                */
    uint16_t charge_current;  /* this+0x82                                     */
    uint32_t state_84;        /* this+0x84  charger state (3=idle / 0xff init) */
    uint16_t fault_flag_86;   /* this+0x86  charger generic fault (0xff init)  */
    uint16_t last_current_88; /* this+0x88  last published charger current     */
    uint16_t last_voltage_8a; /* this+0x8a  last published charger voltage     */
    uint16_t voltage;         /* this+0x8c  pack voltage (mV)                  */
    uint8_t  st_0x8e;         /* this+0x8e  status: frame[0]>>5 & 1            */
    uint8_t  ins_det;         /* this+0x8f  INS-DET: frame[6]>>5 & 1           */
    uint8_t  ins_det_fail;    /* this+0x90  INS-DET-FAIL: frame[6]>>4 & 1      */
    uint8_t  charging;        /* this+0x91  charging flag: frame[0]>>7         */
    uint8_t  fault_uvp_92;    /* this+0x92  UVP/empty-fault derived flag       */
    uint8_t  connected_93;    /* this+0x93  charger-connected latch            */
    uint8_t  identify_sent_94;/* this+0x94  charger-identify request sent      */
    uint8_t  state_nibble_95; /* this+0x95  battery state nibble: frame[6]&0xf */
    uint8_t  fw_update_96;    /* this+0x96  charger FW-update in progress      */
    uint8_t  mode_count_97;   /* this+0x97  "mode 2" sample counter (->0x14)   */
    uint8_t  retry_count_98;  /* this+0x98  charge-retry counter (0..3)        */
};

/* ---- framework helpers used by this module (modelled) ----------------- */

/*
 * Publish-bool sink used by the two flag decoders — OEM 0x13e220. The OEM takes
 * a runtime std::string topic + a bool and writes it into the OD override table
 * (creating the table on first use). Modelled as a named-bool publish.
 */
void od_pub_named_bool(void *ipc, const char *topic, bool value);

/*
 * OEM 0x13df10 / 0x13e860 / 0x13eaa0 — OD override table query / clear-by-key /
 * set-by-key. Used by battery_status_decode (clear the "charging" override when
 * not charging) and by charger_decode_mode (the retry-key dance).
 */
bool od_override_query(const char *key);     /* 0x13df10 */
void od_override_erase(const char *key);     /* 0x13e860 */
void od_override_mark(const char *key);      /* 0x13eaa0 */

/*
 * OD signal registration (the std::vector<OdRegistration> built in Monitor_ctor).
 * `decode` is the std::function target invoked with the 8 raw CAN payload bytes.
 */
typedef void (*od_decode_fn)(struct Monitor *m, const uint8_t *frame);
void od_register_signal(struct Monitor *m, const char *signal, od_decode_fn decode);

/* Subscribe `handler(ctx, payload)` to an OD/MQTT topic (OEM ipc vtable+0x10). */
typedef void (*od_topic_fn)(struct Monitor *m, const void *payload);
void od_subscribe(struct Monitor *m, const char *topic, od_topic_fn handler);

/*
 * JSON-string compare / substring helpers used by the charger update handlers.
 * od_streq models the OEM std::string == const char* (FUN_00109210 == 0);
 * od_contains_charger models the two std::string::find probes for the charger
 * device-id keys (DAT_0018e248 / DAT_0018e268) in monitor.cpp.
 */
bool od_streq(const char *s, const char *lit);          /* std::string::compare */
bool od_contains_charger(const char *s);                /* find(charger-id keys) */

/* ---- public entry points (OEM addresses in comments) ------------------ */

void Monitor_ctor(struct Monitor *m, void *od, void *ipc,
                  void *timer_factory, void *power_control);      /* 0x129520 */

/* charger-connected payload {connected,voltage=f[0],current=f[1]}. */
void monitor_charger_connected_publish(struct Monitor *m,
                                       const uint16_t *frame);    /* 0x128cc4 */
void monitor_charger_disconnected(struct Monitor *m);            /* 0x12cde0 */

/* charger device/charger/finished + device/charger/mode handlers. */
void monitor_charger_finished(struct Monitor *m, const void *json); /* 0x12d790 */
void monitor_charger_progress_started (struct Monitor *m, const void *json); /* 0x12d950 */
void monitor_charger_progress_finished(struct Monitor *m, const void *json); /* 0x12d3d0 */

/* device/charger/mode tick: Success/FailRetry/InProgress -> 0/1/2/3. */
uint8_t charger_decode_mode(struct Monitor *m);                  /* 0x1281d0 */

/* the two CAN flag decoders. `frame` points at the 8 raw payload bytes. */
void battery_status_decode (struct Monitor *m, const uint8_t *frame); /* 0x1268d0 */
void battery_warning_decode(struct Monitor *m, const uint8_t *frame); /* 0x1275a0 */

#endif /* MONITOR_H */
