#ifndef BLE_H
#define BLE_H

/*
 * ble.h — shared declarations for the VanMoof BLE application layer.
 *
 * Reconstructed from the OEM image
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (Nordic nRF52-class, Zephyr + SoftDevice Controller; device link base
 * 0x23000). Only VanMoof application code is reconstructed here; Zephyr / the
 * SoftDevice Controller / the nRF SDK are vendor (declared `extern` and
 * satisfied upstream at link time).
 *
 * RAM globals are written as literal-address volatile accesses so the emitted
 * code matches the OEM (which loads the absolute address from a literal pool).
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ------------------------------------------------------------------ globals */

/* BLE message TX in-flight / channel-busy flag. // 0x20009553 */
#define BLE_TX_BUSY        (*(volatile uint8_t *)0x20009553u)

/* Connect / advertising state byte (0 = idle/cleared, 1 = ready). // 0x20007ef5 */
#define BLE_CONNECT_STATE  (*(volatile uint8_t *)0x20007ef5u)

/* ----------------------------------------------- vendor callees (deferred) */
/* Zephyr / SoftDevice Controller link-state checks gating the connect-state
 * byte update. Exact upstream identity TBC; not reconstructed. */
extern int ble_link_check_51b9c(void);  /* vendor // 0x00051b9c */
extern int ble_link_check_51b7c(void);  /* vendor // 0x00051b7c */

/* ------------------------------------------------------------ ble_msg (TX) */
uint8_t ble_msg_tx_busy_get(void);   /* 0x0003f2cc */
void    ble_msg_tx_busy_clear(void); /* 0x0003f2d8 */

/* -------------------------------------------------------------- ble_connect */
void ble_connect_clear_adv_flag(void); /* 0x0003e588 */
void ble_connect_set_ready_flag(void); /* 0x0003e59c */



/* ====================================================================
 * settings + auth (carved batch 1) — appended declarations
 * ==================================================================== */

/* --- types --- */
/* Write callback invoked by settings_read_property_by_path to emit a value;
 * returns >=0 on success or a negative error (sign-extended to short by the
 * caller). // ABI: r0=ctx, r1=buf, r2=len. */
typedef short (*ble_settings_write_cb)(void *ctx, const void *buf, uint32_t len);

/* --- globals --- */
/* settings RAM value buffers, served by settings_read_property_by_path. */
#define SETTINGS_ECU_SERIAL_BUF  (0x20007f17u)   /* ecu_serial, 0x0d bytes */
#define SETTINGS_PUB_KEY_BUF     (0x20007ef7u)   /* pub_key,    0x20 bytes */
#define SETTINGS_BIKE_ID_BUF     (0x20007f24u)   /* bike_id,    0x0d bytes */
/* Auth advertising-payload extra-field gating bytes. // 0x20007db5 / 0x20007db4 */
#define AUTH_ADV_FLAG_A        (*(volatile uint8_t *)0x20007db5u)
#define AUTH_ADV_FLAG_B        (*(volatile uint8_t *)0x20007db4u)
/* Dedicated enable byte; low bit selects the extra field. // 0x20001189 */
#define AUTH_ADV_EXTRA_FIELD   (*(volatile uint8_t *)0x20001189u)
/* Connection slot table base (4 slots, 0xb8 bytes each). // 0x20003a08 */
#define AUTH_CONN_TABLE        (0x20003a08u)
/* 32-byte public-key scratch buffer. // 0x20001199 */
#define AUTH_PUBKEY            (0x20001199u)
/* 13-byte bike-id scratch buffer. // 0x2000118b */
#define AUTH_BIKE_ID           (0x2000118bu)
/* Default bike id ("FACTORY\0..."), rodata constant. // 0x00064457 */
#define AUTH_BIKE_ID_DEFAULT   (0x00064457u)
/* 13-byte id-event staging buffer. // 0x20007dc8 */
#define AUTH_ID_EVENT_BUF      (0x20007dc8u)
/* Auth event queue handle. // 0x200004f8 */
#define AUTH_EVENT_QUEUE       (0x200004f8u)
/* Per-connection auth-state table base (0x40-byte entries). // 0x20003ce8 */
#define AUTH_CONN_STATE_TABLE  (0x20003ce8u)
/* Fixed sync-object init callback planted into each connection slot. // 0x000587e5 (Thumb) */
#define AUTH_CONN_SLOT_INIT_CB ((void *)0x000587e5u)
// Auth event-descriptor pointers (static rodata structs in device image)
#define AUTH_EVT_DESC_DISCONNECT  ((const void *)0x00067260u)   // 0x3e174 literal
#define AUTH_EVT_DESC_CONNECTION  ((const void *)0x00067248u)   // 0x3e188 literal
#define AUTH_EVT_DESC_0X14        ((const void *)0x00067308u)   // 0x3e584 literal
// auth_init_connection_slots RAM handler/argument blocks and slot table
#define AUTH_CONN_HANDLER_ARG     ((void *)0x200008b0u)   // 0x3e498 literal
#define AUTH_CONN_SLOTS           ((void *)0x20003ce8u)   // 0x3e49c literal: 4 x 0x40-byte slots
#define AUTH_SLOT_INIT_FN         ((void *)0x0005880du)   // 0x3e4a0 literal: thumb fn ptr passed to ble_obj_init_61826
#define AUTH_REG_ARG_44978        ((void *)0x2000088cu)   // 0x3e4a4 literal
#define AUTH_REG_ARG_44D5C        ((void *)0x20000860u)   // 0x3e4a8 literal
#define AUTH_REG_ARG_44D94        ((void *)0x2000087cu)   // 0x3e4ac literal
// auth_format_ble_address strings (device rodata)
#define AUTH_STR_PUBLIC           ((const char *)0x000644a7u)   // "public"
#define AUTH_STR_RANDOM           ((const char *)0x000644aeu)   // "random"
#define AUTH_STR_PUBLIC_ID        ((const char *)0x000644b5u)   // "public-id"
#define AUTH_STR_RANDOM_ID        ((const char *)0x000644bfu)   // "random-id"
#define AUTH_STR_HEX_TYPE         ((const char *)0x000644c9u)   // "0x%02x"
#define AUTH_STR_BLE_ADDR_FMT     ((const char *)0x000644d0u)   // "%02X:%02X:%02X:%02X:%02X:%02X (%s)"
// auth_send_disconnect_reason strings (device rodata)
#define AUTH_STR_DISCONNECT_REASON   ((const char *)0x000644f3u)   // "disconnect_reason"
#define AUTH_STR_INVALID_CERT        ((const char *)0x00064505u)   // "invalid certificate/parsing error"
#define AUTH_STR_SERVER_SIG_INVALID  ((const char *)0x00064527u)   // "server signature invalid"
#define AUTH_STR_BIKE_ID_INVALID     ((const char *)0x00064540u)   // "bike id invalid"
#define AUTH_STR_CERT_EXPIRED        ((const char *)0x00064550u)   // "certificate expired"
#define AUTH_STR_CERT_BLACKLISTED    ((const char *)0x00064564u)   // "certificate blacklisted"
#define AUTH_STR_CHALLENGE_INVALID   ((const char *)0x0006457cu)   // "challenge return invalid"
#define AUTH_STR_AUTH_TIMEOUT        ((const char *)0x00064595u)   // "authentication timeout"
#define AUTH_STR_OTHER               ((const char *)0x000645acu)   // "other" (default; 0x3e570 literal)

/* --- vendor callees (deferred) --- */
/* rodata literals (device 0x63000+). Declared as arrays so &name decays to the
 * literal pointer the OEM loads from its constant pool. Satisfied at link. */
extern const char SETTINGS_KEY_TIME[];        /* "time"               // 0x000641d6 */
extern const char SETTINGS_TIME_FMT[];        /* "%u/%u/%u,%u:%u:%u"  // 0x000640a0 */
extern const char SETTINGS_KEY_ECU_SERIAL[];  /* "ecu_serial"         // 0x00064649 */
extern const char SETTINGS_KEY_PUB_KEY[];     /* "pub_key"            // 0x00064657 */
extern const char SETTINGS_KEY_BIKE_ID[];     /* "bike_id"            // 0x00064662 */
/* Value-reader descriptor init: fills the 8-word reader struct with a const
 * vtable/template plus the (arg, arg) source handle. // 0x0004c170 */
extern void settings_value_reader_init(void *reader, uint32_t a1, uint32_t a2);
/* CBOR parser init over a value reader; writes parser scratch + value ctx,
 * returns 0 on success. (TinyCBOR-style) // 0x000602da */
extern int cbor_parser_init(void *reader, int flags, void *parse_out, void *value_ctx);
/* Pull a named byte-string field from a CBOR map (tag 0x60 expected) into dst,
 * bounded by max; returns the field length or -1. // 0x00058c56 */
extern int cbor_map_get_bstr(void *value_ctx, const char *key, void *dst, uint32_t max);
/* scanf-style field extraction (format + varargs of uint*/ /* outputs); returns
 * the number of fields converted. // 0x00050e6c */
extern int settings_sscanf(const void *src, const char *fmt, ...);
/* Validate a tm-style block (sec/min/hour/mday/mon/year) and apply it to the
 * RTC; returns 0 on success or -EINVAL (-0x16). // 0x0005f590 */
extern int settings_apply_time(void *tm);
/* Path tokenizer: returns length of the leading segment (up to '/', '=', or
 * NUL) and stores the remainder pointer (segment+1) via *rest. // 0x00059bfa */
extern int settings_path_token_len(const char *path, void *rest);
/* Bounded string compare (strncmp-like over n bytes); returns 0 on match.
 * // 0x00061e98 */
extern int settings_strncmp(const char *a, const char *b, int n);
/* Toolchain/libc fill helper: set `len` bytes at `dst` to `val`. */
extern void vm_memset_61e62(void *dst, int val, uint32_t len);   /* vendor // 0x00061e62 */
/* Toolchain/libc bounded copy: copy `n` bytes from `src` to `dst` (cap = `n_max`). */
extern void vm_memcpy_bounded_61e3c(void *dst, const void *src, uint32_t n, uint32_t n_max);  /* vendor // 0x00061e3c */
/* Zephyr/SDK sync-object (mutex/queue) init for a connection slot. */
extern void vm_sync_object_init_61826(void *obj, void *cb, uint32_t arg);  /* vendor // 0x00061826 */
/* Queue helpers (thunk 0x6148c -> 0x613de): is the queue's lock held / release it. */
extern int  vm_queue_lock_held_613de(void *queue);    /* vendor // 0x000613de (via thunk 0x6148c) */
extern void vm_queue_lock_release_61490(void *queue); /* vendor // 0x00061490 */
/* Vendor queue-post / event-state routine (interior of 0x00044554). */
extern uint32_t vm_queue_post_4fdd4(void *queue, uint32_t a, uint32_t b, uint32_t c);  /* vendor // 0x0004fdd4 */
/* Connection link-ready check. */
extern int vm_link_ready_58858(uint32_t conn);  /* vendor // 0x00058858 */
/* Map a connection handle to a slot index (0..255). */
extern uint32_t vm_conn_handle_to_index_446f4(uint32_t conn);  /* vendor // 0x000446f4 */
extern void *ble_event_alloc(uint32_t size);  /* vendor // 0x00059e04 */
extern int ble_snprintf(char *buf, uint32_t size, const char *fmt, ...);  /* vendor // 0x00058f24 */
extern void ble_strlcpy(char *dst, const char *src);  /* vendor // 0x00061e88 */
extern void ble_json_writer_init(void *hdr, void *buf, uint32_t cap);  /* vendor // 0x0004c1a8 */
extern void ble_json_begin(void *writer, void *hdr, uint32_t flags);  /* vendor // 0x0005fe78 */
extern int ble_json_open(void *writer, void *state, uint32_t arg);  /* vendor // 0x0005ff14 */
extern void ble_json_str(void *writer, const char *str);  /* vendor // 0x00058938 */
extern int ble_json_close(void *writer, void *state);  /* vendor // 0x0005ff32 */
extern void ble_msg_transmit(uint32_t a, uint32_t type, void *buf, uint32_t len, uint32_t e, uint32_t f);  /* vendor // 0x00058a50 */
extern void ble_event_post(int event);  /* vendor // 0x00040d98 */
extern void ble_reg_460d0(void *arg);  /* vendor // 0x000460d0 */
extern void ble_reg_44978(void *arg);  /* vendor // 0x00044978 */
extern void ble_reg_44d5c(void *arg);  /* vendor // 0x00044d5c */
extern void ble_reg_44d94(void *arg);  /* vendor // 0x00044d94 */
extern void ble_obj_init_61826(void *obj, void *init_fn, uint32_t arg);  /* vendor // 0x00061826 */

/* --- prototypes --- */
void settings_parse_time_pubkey(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t src_a, uint32_t src_b);  /* 0x0003c660 */
int  settings_read_property_by_path(const char *path, int key, ble_settings_write_cb write_cb, void *cb_ctx);  /* 0x0003e784 */
/* ---------------------------------------------------------------- auth */
uint8_t  auth_adv_extra_field_enabled(void);            /* 0x0003d688 */
void     auth_init_connection_table(void);              /* 0x0003dfb8 */
void     auth_copy_pubkey_32(const uint32_t *src);      /* 0x0003dff8 */
void     auth_copy_bike_id(const void *src, uint32_t len);   /* 0x0003e020 */
uint32_t auth_submit_id_event(const void *src, uint32_t len); /* 0x0003e11c */
uint32_t auth_check_connection_state(uint32_t conn);    /* 0x0003e18c */
int auth_alloc_disconnect_event(void);  /* 0x0003e164 */
int auth_alloc_connection_event(void);  /* 0x0003e178 */
int auth_alloc_event_0x14(void);  /* 0x0003e574 */
void auth_format_ble_address(const uint8_t *addr, char *out);  /* 0x0003e2d8 */
void auth_init_connection_slots(void);  /* 0x0003e45c */
void auth_send_disconnect_reason(uint32_t conn_handle, uint8_t flag, uint32_t reason);  /* 0x0003e4b0 */






/* ====================================================================
 * auth core (carved batch 2) — appended declarations
 * ==================================================================== */

/* --- types --- */
/* Parsed "connection command" control message. The handler only inspects the
 * descriptor (+4), the subcommand byte (+8), and the flags word (+0xc). */
typedef struct {
    uint32_t reserved0;   /* +0x0 */
    uint32_t descriptor;  /* +0x4: must equal AUTH_CONN_CMD_DESC */
    uint8_t  command;     /* +0x8: subcommand 0..4 */
    uint8_t  pad9;
    uint8_t  pad10;
    uint8_t  pad11;
    uint32_t flags;       /* +0xc: bit0 challenge variant, bit1 suppress reply, bit2 suppress publish */
} auth_conn_msg_t;

/* --- globals --- */
/* Static "connection command" message descriptor; msg->descriptor (+4) must
 * equal this to be handled. Points at a rodata descriptor struct. // 0x00067278 */
#define AUTH_CONN_CMD_DESC        (0x00067278u)
/* FindMy-enabled flag, raised once serial/pub-key/bike-id are installed into
 * the secure session. // 0x20007db6 */
#define AUTH_FINDMY_ENABLED_FLAG  (*(volatile uint8_t *)0x20007db6u)
/* 3-byte challenge seed copied into the case-0 frame ({0x01,0x32,0x00}). rodata. // 0x00062616 */
#define AUTH_TIME_PUBKEY_SEED     ((const uint8_t *)0x00062616u)
/* Connection-command certificate-challenge descriptor (rodata table). // 0x00062990 */
#define AUTH_CERT_DESC            ((void *)0x00062990u)
/* rodata key string "findmy" used both as a JSON key and a bus topic. // 0x00064168 */
#define AUTH_FINDMY_KEY           ((const char *)0x00064168u)
/* rodata bus/MQTT topic "findmy/enable". // 0x000642e6 */
#define AUTH_FINDMY_ENABLE_TOPIC  ((const char *)0x000642e6u)
/* Literal field value emitted by ble_json_add_field_5fe92 in case 0. // 0x3d838 literal */
#define AUTH_FINDMY_FIELD_CONST   (0x01010000u)
/* Provisioning-topic match handler passed to ble_bus_publish_40618 (Thumb code
 * pointer; this is findmy_match_provisioning_topic). // 0x0003d234 */
#define AUTH_PROV_TOPIC_HANDLER   ((void *)0x0003d234u)
/* Connection-state descriptor installed by auth_send_connection_state. // 0x000629a4 */
#define AUTH_STATE_DESC           ((void *)0x000629a4u)
/* Cached session-field source buffers pushed into the secure session. */
#define AUTH_SESSION_SERIAL_SRC   ((void *)0x20007994u)   /* serial   // 0x20007994 */
#define AUTH_SESSION_PUBKEY_SRC   ((void *)0x20007da4u)   /* pub key  // 0x20007da4 */
#define AUTH_SESSION_BIKEID_SRC   ((void *)0x200079a4u)   /* bike id  // 0x200079a4 */
/* State-report handler passed to ble_bus_publish_40618 (Thumb code pointer,
 * un-carved interior function). // 0x0003d1c4 */
#define AUTH_STATE_REPORT_HANDLER ((void *)0x0003d1c4u)
/* Fixed 64-bit baseline used by auth_parse_certificate_challenge to reject an
 * implausibly-early clock (low word; high word is 0x18c). // 0x0003db20 literal */
#define AUTH_VALIDITY_MIN_BASELINE  0x0000018cc251f3ffLL
/* Static work-item descriptor planted into the internal auth event at +4.
 * rodata struct in the device image. // 0x00067218 (0x3db30 literal) */
#define AUTH_INTERNAL_EVENT_DESC  ((const void *)0x00067218u)

/* --- vendor callees (deferred) --- */
extern void *ble_conn_get_address(void *conn); /* vendor // 0x5c9a4 (returns conn+0x90) */
extern int ble_conn_is_valid(void *conn); /* vendor // 0x58858 */
extern int ble_conn_get_handle_id(void *conn, uint16_t *out_handle); /* vendor // 0x5b906 */
extern int ble_msg_alloc(uint16_t type, int len); /* vendor // 0x42418 */
extern void *ble_msg_reserve(int msg_buf, short n); /* vendor // 0x5ef52 */
extern int ble_msg_finalize(uint16_t type, int msg, int *out_msg); /* vendor // 0x4248c */
extern void ble_msg_free(int msg); /* vendor // 0x4860c */
extern void ble_timer_init(int timer, int base, unsigned int a, int b, unsigned int period, int d); /* vendor // 0x50920 */
extern void ble_timer_stop(int timer); /* vendor // 0x6183e */
extern int ble_conn_ref(void *conn); /* vendor // 0x5c7e8 */
extern void ble_conn_unref(void *conn); /* vendor // 0x5c81e */
/* Secure-session state register read: returns (state & 7) >> 2 (session-active
 * predicate); DMB-fenced. // 0x0004e9e4 */
extern uint32_t auth_secure_state_get_4e9e4(void);
/* Tear down the secure session (exclusive-access bit dance); 0 on success. // 0x0004eb28 */
extern int auth_secure_session_teardown_4eb28(void);
/* (Re)generate the local certificate / challenge response; 0 on success. // 0x0004e418 */
extern int auth_certificate_generate_4e418(void);
/* Apply/verify a certificate challenge from the 3-byte seed frame against the
 * descriptor; 0 on success. // 0x0004e9fc */
extern int auth_certificate_challenge_apply_4e9fc(void *frame, void *cert_desc);
/* Install a connection-state descriptor into the secure-session slot. // 0x0004e298 */
extern int auth_secure_session_install_4e298(void *desc);
/* Force-reset the secure session (clears state, DMB-fenced). // 0x00042ca8 */
extern void auth_secure_session_reset_42ca8(void);
/* Push the cached serial into the secure session (len 0x10 field). // 0x0004e8b8 (thunk 0x00060736) */
extern int auth_session_set_serial_4e8b8(void *src);
/* Push the cached public key into the secure session (len 0x10 field). // 0x0004e8d4 */
extern int auth_session_set_pubkey_4e8d4(void *src);
/* Push the cached bike id into the secure session (len 0x400 field). // 0x0004e8f0 */
extern int auth_session_set_bike_id_4e8f0(void *src);
/* Read up to *count 7-byte session records into out; updates *count. // 0x000433c0 */
extern int findmy_read_records_433c0(void *out, uint32_t *count);
/* Advance/process one session record; <0 error, 1 = done, else continue. // 0x000433e4 */
extern int findmy_process_record_433e4(int sel);
/* Queue a findmy work item with type byte 0 and the given code. // 0x0005874c */
extern void findmy_enqueue_event_0(uint32_t code);
/* Queue a findmy work item with type byte 1 and the given code. // 0x00058732 */
extern void findmy_enqueue_event_1(uint32_t code);
/* Publish a payload to a bus/MQTT topic (acquires the publish lock, dispatches).
 * The 3rd register arg is the payload byte length (0 for a NULL payload), not a
 * flag. // 0x00040558 */
extern uint32_t ble_msg_publish_40558(const char *topic, const void *payload, uint32_t len);
/* Publish to the in-process subscriber bus, invoking each registered handler. // 0x00040618 */
extern uint32_t ble_bus_publish_40618(uint32_t arg0, void *handler, uint32_t flag);
/* Build + send a small JSON state message of the given type (calls 0x58766). // 0x0005876e */
extern void ble_send_state_msg_5876e(uint32_t msg_type);
/* Append the connection-state value (state + 0x14) to a JSON writer. // 0x00058766 */
extern void ble_json_add_state_58766(void *writer, uint8_t state);
/* Emit a JSON key string into a writer. // 0x000587aa */
extern void ble_json_emit_key_587aa(void *writer, const char *key);
/* Append a signed integer JSON field to a writer (sign = value>>31 control). // 0x0005fe92 */
extern void ble_json_add_field_5fe92(void *writer, uint32_t tag, uint32_t value, uint32_t sign);
/* System reset / fault entry; raises BASEPRI and loops (no return). // 0x0003ffac */
__attribute__((noreturn)) extern void ble_system_reset_3ffac(uint32_t mode);
/* Server certificate-challenge fields. The challenge message is a 0x40-byte
 * signature blob over the trailing CBOR body. cbor_verify_signature recomputes
 * the MAC/signature with `key` and returns 0 only when it matches `msg`.
 * (CC310 / vendor crypto.) */
extern uint32_t cbor_verify_signature(const uint8_t *msg, const uint8_t *body, uint32_t body_len, const uint8_t *key);  /* vendor // 0x0005606c */
/* CBOR map field extractors (TinyCBOR-style). u32/u64 read an integer field by
 * key and return 0 / -1; bstr_len copies a byte-string field (bounded by max,
 * 0 = unbounded) and returns its length or a negative error. // vendor */
extern uint32_t cbor_map_get_u32(void *value_ctx, const uint8_t *key, uint32_t *out);   /* vendor // 0x00058b80 */
extern uint32_t cbor_map_get_u64(void *value_ctx, const uint8_t *key, uint32_t out[2]); /* vendor // 0x00058bb8 */
extern int      cbor_map_get_bstr_len(void *value_ctx, const uint8_t *key, void *dst, uint32_t max); /* vendor // 0x00058c9c */
/* Read the current wall-clock as a 64-bit millisecond value into out[0..1];
 * returns 0 on success or a negative error when the clock is unavailable. */
extern uint32_t cbor_get_now_ms(uint32_t out[2]);  /* vendor // 0x0005f5fa */
/* CBOR key / OID descriptor blobs (device rodata, 2-byte header + payload).
 * Declared as arrays so &name decays to the OEM literal-pool pointer. */
extern const uint8_t AUTH_CBOR_KEY_ISSUER[];        /* "...crypto@5002a000" key blob // 0x00064c01 */
extern const uint8_t AUTH_CERT_OID_A[];             /* recognised DER OID (14 bytes) // 0x00064b1c */
extern const uint8_t AUTH_CERT_OID_B[];             /* recognised DER OID (14 bytes) // 0x000646b2 */
extern const uint8_t AUTH_CBOR_KEY_AUTH_MODULE[];   /* "auth_module" field key // 0x00064472 */
extern const uint8_t AUTH_CBOR_KEY_FMNA_SOUND[];    /* "fmna_sound" field key  // 0x00064da3 */
extern const uint8_t AUTH_CBOR_KEY_CMD[];           /* "cmd" field key         // 0x000640b0 */
extern const uint8_t AUTH_CBOR_KEY_FMNA_SERIAL[];   /* "fmna_serial_number" key // 0x00064d90 */
extern const uint8_t AUTH_CBOR_KEY_CONNECTION_ID[]; /* "connection_id" field key // 0x00064422 */
/* 32-byte server verification public key (== AUTH_PUBKEY 0x20001199), passed to
 * cbor_verify_signature. Declared as an array for &name pointer decay. */
extern const uint8_t AUTH_VERIFY_PUBKEY[];          /* // 0x20001199 (AUTH_PUBKEY) */

/* --- prototypes --- */
void auth_handle_disconnect(void *conn, uint8_t reason); /* 0x3e350 */
void auth_handle_connect(void *conn, int status); /* 0x3e3a8 */
void *findmy_alloc_work_item(void);                 /* 0x0003d220 */
void  findmy_send_state_report(void);               /* 0x0003d47c */
void *findmy_match_provisioning_topic(const char *topic, uint32_t len);  /* 0x0003d234 (registered as a bus subscriber; also AUTH_PROV_TOPIC_HANDLER) */
uint32_t auth_handle_connection_command(void *msg); /* 0x0003d6b0 */
void     auth_send_connection_state(void);          /* 0x0003d840 */
void auth_parse_certificate_challenge(uint32_t conn_handle, const uint8_t *msg, uint32_t len);  /* 0x0003d920 */




/* ====================================================================
 * ble connect / char / message (carved batch 3) — appended declarations
 * ==================================================================== */

/* --- types --- */
/* 24-byte static flash-region descriptor (FTP_BLOB_TABLE entries; also the
 * currently-selected FTP_ACTIVE_DESC target). */
typedef struct ftp_blob_desc {
    uint8_t     flag;     /* +0x00: 0 = erase per-page on write, non-0 = erase whole region on open */
    uint8_t     pad1;
    uint8_t     pad2;
    uint8_t     pad3;
    const char *name;     /* +0x04: region name ("app_update" / "fmna_blob") */
    uint32_t    word8;    /* +0x08 */
    uint32_t    offset;   /* +0x0c: flash base offset of the region */
    uint32_t    size;     /* +0x10: region size in bytes */
    uint32_t    callback; /* +0x14: void(*)(int status) completion callback (0 = none) */
} ftp_blob_desc_t;

/* Zephyr flash device + driver API (vendor structs; only the touched fields). */
struct ble_flash_device;
struct ble_flash_api {
    int (*read)(const struct ble_flash_device *dev, uint32_t off, void *dst, uint32_t len);        /* +0x00 */
    int (*write)(const struct ble_flash_device *dev, uint32_t off, const void *src, uint32_t len); /* +0x04 */
    int (*erase)(const struct ble_flash_device *dev, uint32_t off, uint32_t len);                  /* +0x08 */
};
struct ble_flash_device {
    const char *name;                  /* +0x00 */
    uint32_t    config;                /* +0x04 */
    const struct ble_flash_api *api;   /* +0x08 */
    void       *data;                  /* +0x0c */
};

/* BLE command dispatch/broadcast table entry (12 bytes, rodata): a 16-bit
 * command id, a Thumb handler pointer, and a per-command argument pointer. */
typedef struct {
    uint16_t id;       /* +0x0 */
    uint16_t pad;      /* +0x2 */
    uint32_t handler;  /* +0x4: Thumb code pointer (raw word); called handler(arg, id, ctx) */
    uint32_t arg;      /* +0x8: per-command argument word (usually a rodata name pointer) */
} ble_cmd_entry_t;
typedef void (*ble_cmd_handler_fn)(uint32_t arg, uint16_t id, uint32_t ctx);

/* --- globals --- */
/* connect / advertise */
#define BLE_CONNECT_MSG_HANDLE   (*(volatile int *)0x20007df7u)      /* EXT_API connect message slot id // 0x20007df7 */
#define BLE_CONNECT_FSM_MODE     (*(volatile uint8_t *)0x20007ed5u)  /* pairing FSM mode (0 idle,1 begin,2 ready) // 0x20007ed5 */
#define BLE_CONNECT_PAYLOAD_BUF  ((void *)0x20007dd5u)               /* 32-byte staged connect payload // 0x20007dd5 */
#define BLE_CONNECT_PAYLOAD_LEN  (*(volatile uint32_t *)0x200008b8u) /* staged connect payload length // 0x200008b8 */
#define BLE_COSE_AUDIENCE        ((const char *)0x000645ddu)         /* "nl.samsonit.vanmoofapp" // 0x000645dd */
#define BLE_CONNECT_URL_BIKE_ID  ((const char *)0x000645f4u)         /* "vmf://connect?bike_id=" // 0x000645f4 */
#define BLE_CONNECT_URL_ECU      ((const char *)0x0006460bu)         /* "vmf://connect?main_ecu_serial=" // 0x0006460b */
#define BLE_BUS_TOPIC_VM         ((const char *)0x0006466au)         /* in-process bus topic "vm" // 0x0006466a */

/* GATT characteristic value buffers / publish topics */
#define BLE_PUBKEY_VALUE_BUF     (0x200076f7u)                /* pub_key GATT value (32B); != SETTINGS_PUB_KEY_BUF/AUTH_PUBKEY // 0x200076f7 */
#define BLE_TOPIC_ECU_SERIAL     ((const char *)0x00064646u)  /* "vm/ecu_serial" // 0x00064646 */
#define BLE_TOPIC_PUB_KEY        ((const char *)0x00064654u)  /* "vm/pub_key"    // 0x00064654 */
#define BLE_TOPIC_BIKE_ID        ((const char *)0x0006465fu)  /* "vm/bike_id"    // 0x0006465f */

/* ftp_command (firmware / flash-blob transfer over BLE) */
#define BLE_FLASH_DEV        ((const struct ble_flash_device *)0x000622b8u)  /* "flash-controller@4001e000" // 0x000622b8 */
#define FTP_REPLY_BUF        (0x20007f31u)                        /* static CBOR reply buffer (cap 0x40) // 0x20007f31 */
#define FTP_ACTIVE_DESC      (*(volatile uint32_t *)0x20005298u)  /* active flash-blob descriptor ptr (0 = idle) // 0x20005298 */
#define FTP_NAME_BUF         (0x20007f71u)                        /* inbound "name" scratch // 0x20007f71 */
#define FTP_DATA_BUF         (0x20007f91u)                        /* inbound "data" scratch // 0x20007f91 */
#define FTP_BLOB_TABLE       (0x000629acu)                        /* 2 x 24-byte ftp_blob_desc (app_update, fmna_blob) // 0x000629ac */
#define FTP_KEY_CMD          ((const char *)0x000640b2u)   /* "cmd"        */
#define FTP_KEY_SILENT       ((const char *)0x0006466du)   /* "silent"     */
#define FTP_KEY_NAME         ((const char *)0x00064674u)   /* "name"       */
#define FTP_KEY_SIZE         ((const char *)0x000646bau)   /* "size"       */
#define FTP_KEY_INDEX        ((const char *)0x000646bfu)   /* "index"      */
#define FTP_KEY_DATA         ((const char *)0x000646c5u)   /* "data"       */
#define FTP_KEY_CRC          ((const char *)0x000646cau)   /* "crc"        */
#define FTP_KEY_STATUS       ((const char *)0x000646ceu)   /* "status"     */
#define FTP_KEY_CHUNK_SIZE   ((const char *)0x000646b4u)   /* "chunk_size" */
#define FTP_KEY_N            ((const char *)0x000642b8u)   /* "n" (cmd-4 block count) */
#define FTP_STR_APP_UPDATE   ((const char *)0x00064679u)   /* "app_update" */
#define FTP_STR_FMNA_BLOB    ((const char *)0x000646aau)   /* "fmna_blob"  */
#define FTP_STR_COMPARING    ((const char *)0x00064684u)   /* "Comparing %s with %s (%d characters)\n" */

/* command dispatch (incoming requests) */
#define BLE_CMD_KEY_TPC             ((const uint8_t *)0x000646e1u)  /* "tpc" command-id key // 0x000646e1 */
#define BLE_CMD_KEY_SUB             ((const uint8_t *)0x000646e5u)  /* "sub" subscribe-flag key // 0x000646e5 */
#define BLE_CMD_DISPATCH_TABLE      ((const void *)0x000671e8u)     /* 2 x 12-byte entries // 0x000671e8 */
#define BLE_CMD_DISPATCH_TABLE_END  ((const void *)0x00067200u)
#define BLE_CMD_BROADCAST_TABLE     ((const void *)0x00067108u)     /* 14 x 12-byte entries // 0x00067108 */
#define BLE_CMD_BROADCAST_TABLE_END ((const void *)0x000671b0u)

/* comm-port (SPI-bridge) transport */
#define BLE_COMM_LOCK              (0x20001e38u)            /* comm-port lock object (paired with spi_bridge_unlock) // 0x20001e38 */
#define BLE_COMM_FRAME_BUF         (0x20008191u)            /* shared framed-packet buffer (cap 0x406) // 0x20008191 */
#define BLE_COMM_PIPE              (0x20001f18u)            /* comm-port pipe / ring object // 0x20001f18 */
#define BLE_COMM_NOTIFY_PTR        (0x2000529cu)            /* ptr-to-consumer-notify object // 0x2000529c */
#define BLE_DEVICE_PROVISION_WORD  (*(volatile uint32_t *)0x10001208u)  /* UICR config word (0xFFFFFFFF = unprovisioned) // 0x10001208 */

/* --- vendor callees (deferred) --- */
/* toolchain / libc */
extern uint32_t vm_strlen_36d1c(const char *s);  /* vendor // 0x00036d1c (strlen) */
extern void    *vm_memcpy_61e20(void *dst, const void *src, uint32_t len);  /* vendor // 0x00061e20 (forward bytewise memcpy) */
extern int      vm_memcmp_61e00(const void *a, const void *b, uint32_t n);  /* vendor // 0x00061e00 (memcmp) */
extern int      aeabi_d2iz(uint32_t lo, uint32_t hi);  /* vendor // 0x000234e0 (__aeabi_d2iz) */
/* connect / secure-session / EXT_API */
extern int  ble_cose_sign_4a7ec(const char *audience, int audience_len, const void *body, int body_len);  /* vendor // 0x0004a7ec (COSE/CWT signer) */
extern int  ble_msg_reserve_len_59e16(int handle, uint32_t *len);  /* vendor // 0x00059e16 (GATT length-header reserve) */
extern int  ble_msg_send_51670(int handle, uint32_t len);  /* vendor // 0x00051670 (EXT_API send/commit) */
extern void ble_json_open_5fe82(void *json_state);  /* vendor // 0x0005fe82 (JSON open-container; -> 0x0005fe1a) */
extern int  ble_secure_session_start_51b0c(void *handler_cb, int arg);  /* vendor // 0x00051b0c (secure-session bring-up) */
/* ftp_command: logger / crc / CBOR / flash driver */
extern void     ble_log_58f0a(const char *fmt, ...);  /* vendor // 0x00058f0a (printf-style logger) */
extern uint32_t ble_crc16_58d72(const void *data, int len);  /* vendor // 0x00058d72 (CRC-16) */
extern int      cbor_map_find_6042e(void *map_value, const char *key, void *out_cursor);  /* vendor // 0x0006042e */
extern int      cbor_read_int_58a12(void *cursor, uint32_t *out);  /* vendor // 0x00058a12 */
extern int      cbor_get_bstr_len_6041e(void *cursor, uint32_t *len);  /* vendor // 0x0006041e */
extern int      cbor_copy_bstr_4c1bc(void *cursor, void *dst, uint32_t *len, int flag);  /* vendor // 0x0004c1bc */
extern void     ble_json_emit_key_58a36(void *writer, const char *key);  /* vendor // 0x00058a36 (distinct from 0x587aa) */
/* command dispatch: CBOR field readers */
extern uint32_t cbor_map_get_f64(void *value_ctx, const uint8_t *key, uint32_t out[2]);  /* vendor // 0x00058bf6 */
extern uint32_t cbor_map_get_bool(void *value_ctx, const uint8_t *key, uint32_t *out);   /* vendor // 0x00058c2c */
/* comm-port transport primitives */
extern int  ble_comm_lock_take_4f478(void *lock, uint32_t budget, uint32_t a, uint32_t b);  /* vendor // 0x0004f478 */
extern int  ble_cobs_encode_61c50(const void *src, uint32_t len, void *dst, uint32_t cap);  /* vendor // 0x00061c50 (COBS framing) */
extern int  ble_comm_pipe_write_4f7f0(void *pipe, const void *buf, uint32_t n, uint32_t *out, uint32_t min, uint32_t a, uint32_t budget, uint32_t flags);  /* vendor // 0x0004f7f0 */
extern int  ble_comm_notify_58af4(void *obj, uint32_t mode);  /* vendor // 0x00058af4 */

/* --- VanMoof callees not in this batch (forward decls; carved/declared later) --- */
void     spi_bridge_unlock(void);                         /* 0x0003ef10 (SPI-bridge lock release) */
uint32_t ble_msg_publish_clear_59bac(const char *topic);  /* 0x00059bac (publish topic with NULL payload) */
void     ble_announce_command_id(uint16_t id);            /* 0x00058aa8 (broadcast one command id) */

/* --- prototypes (carved batch 3) --- */
void     ble_send_signed_connect_payload(const void *data, int data_len, const void *prefix, int prefix_len);  /* 0x0003e5b0 */
uint32_t ble_connect_state_machine(void *msg);                             /* 0x0003e640 */
void     ble_advertise_bike_id_payload(const void *src, uint32_t len);     /* 0x0003e6e0 */
void     ble_advertise_ecu_serial_payload(const void *src, uint32_t len);  /* 0x0003e72c */
int      ble_secure_session_init(void);                                    /* 0x0003e778 */
uint32_t ble_build_connect_advert_payload(void);                           /* 0x0003e948 */
int      ble_char_write_value_26(const void *data, uint32_t len);          /* 0x0003e818 */
uint32_t ble_build_const_response_13(void *dst);                           /* 0x0003e860 */
void     ble_send_32byte_value(const void *data);                          /* 0x0003e880 */
int      ble_char_write_value_13(const void *data, uint32_t len);          /* 0x0003e8c8 */
int      ble_bike_id_present(void);                                        /* 0x0003e924 */
void     ble_ftp_command_handler(uint32_t a0, uint32_t a1, uint32_t conn, uint32_t src_a, uint32_t src_b);  /* 0x0003e9a0 */
void     ble_message_dispatch_by_id(uint32_t a0, uint32_t a1, uint32_t ctx, uint32_t src, uint32_t len);    /* 0x0003edbc */
int      ble_msg_send(const void *src, uint32_t len);                      /* 0x0003f210 */
void     ble_uicr_write_init_flag(void);                                   /* 0x0003f2e4 */




/* ====================================================================
 * spi_bridge + findmy_glue (carved batch 4) — appended declarations
 * ==================================================================== */

/* --- globals --- */
/* FindMy connection slot table: 12-byte entries {auth(+0), enc(+1), word4(+4),
 * conn(+8)} indexed by the connection slot index. // 0x20005268 */
#define FINDMY_CONN_SLOTS        (0x20005268u)
/* Static framed-message build buffer used by findmy_handle_conn_rx. // 0x200073c0 */
#define FINDMY_MSG_BUILD_BUF     (0x200073c0u)
/* Scratch frame buffer used by findmy_forward_peer_payload. // 0x200070d2 */
#define FINDMY_FWD_SCRATCH       (0x200070d2u)
/* Base of the 3 x 0x338-byte FindMy connection records (record base is -0x8 from
 * this). // 0x20003358 */
#define FINDMY_CONN_SLOT_BASE    (0x20003358u)
/* RAM word holding the FindMy message-queue handle. // 0x20001eb0 */
#define FINDMY_MSG_QUEUE_ADDR    (0x20001eb0u)
/* Runtime fmna provisioning-topic table: 3 string pointers, populated when the
 * provisioning subscriptions are registered. // 0x20000830 */
#define FINDMY_PROV_TOPIC_TABLE  (0x20000830u)

/* FindMy event-class descriptor structs (rodata; each begins with a name ptr). */
#define FINDMY_EVT_DESC_CONN     (0x00067248u)   /* "conn_event" (== AUTH_EVT_DESC_CONNECTION) // 0x00067248 */
#define FINDMY_EVT_DESC_AUTH     (0x00067200u)   /* "auth_event" (== addr of BLE_CMD_DISPATCH_TABLE_END) // 0x00067200 */
#define FINDMY_EVT_DESC_SYNC     (0x00067320u)   /* "sync_event" // 0x00067320 */

/* FindMy JSON field-key strings (device rodata). */
#define FINDMY_KEY_CID           ((const char *)0x0006416fu)   /* "cid"         */
#define FINDMY_KEY_RSSI          ((const char *)0x00064173u)   /* "rssi"        */
#define FINDMY_KEY_ERR           ((const char *)0x00064178u)   /* "err"         */
#define FINDMY_KEY_INTERVAL      ((const char *)0x0006417cu)   /* "interval"    */
#define FINDMY_KEY_RX_MAX_TIME   ((const char *)0x00064185u)   /* "rx_max_time" */
#define FINDMY_KEY_RX_MAX_LEN    ((const char *)0x00064191u)   /* "rx_max_len"  */
#define FINDMY_KEY_ENC           ((const char *)0x0006419cu)   /* "enc"         */
#define FINDMY_KEY_LATENCY       ((const char *)0x000640ecu)   /* "latency"     */
#define FINDMY_KEY_TX_MAX_LEN    ((const char *)0x000640bbu)   /* "tx_max_len"  */
#define FINDMY_KEY_TX_MAX_TIME   ((const char *)0x000640c6u)   /* "tx_max_time" */
#define FINDMY_KEY_TIMEOUT       ((const char *)0x000645a4u)   /* "timeout"     */
#define FINDMY_KEY_AUTH          ((const char *)0x0006434du)   /* "auth"        */
#define FINDMY_KEY_REASON        ((const char *)0x000644feu)   /* "reason"      */

/* findmy_match_provisioning_topic: rodata "/1" slot-suffix matched at topic tail. // 0x00064267 */
#define FINDMY_PROV_TOPIC_SUFFIX     ((const char *)0x00064267u)
/* findmy_send_state_report JSON field keys (rodata). */
#define FINDMY_STATE_KEY_READY       ((const char *)0x00064287u)  /* "ready"       */
#define FINDMY_STATE_KEY_ENABLED     ((const char *)0x0006428du)  /* "enabled"     */
#define FINDMY_STATE_KEY_PAIRED      ((const char *)0x000642a8u)  /* "paired"      */
#define FINDMY_STATE_KEY_PAIRING     ((const char *)0x00064dd2u)  /* "pairing"     */
#define FINDMY_STATE_KEY_PROVISIONED ((const char *)0x00064295u)  /* "provisioned" */

/* --- vendor callees (deferred) --- */
/* SPI-bridge comm-port lock release primitive (Zephyr give/unblock). */
extern int comm_lock_release_4f568(void *lock);  /* vendor // 0x0004f568 */
/* k_msgq_put-style enqueue of a fixed-size item (basepri-fenced ring send). Shared
 * with the (deferred) spi_bridge consumer thread. */
extern int comm_msgq_put_4f318(void *q, const void *item, uint32_t a, uint32_t b);  /* vendor // 0x0004f318 */
/* nRF52 POWER->RESETREAS accessors: read+remap the latched reset cause, and clear it. */
extern uint32_t ble_resetreas_read_5f34e(uint32_t *out);  /* vendor // 0x0005f34e */
extern uint32_t ble_resetreas_clear_5f388(void);          /* vendor // 0x0005f388 */
/* JSON writer primitives (same vendor family as ble_json_emit_key_587aa/_5fe92):
 * key emit, and small-integer field append. */
extern void ble_json_key_585a2(void *writer, const char *key);   /* vendor // 0x000585a2 */
extern void ble_json_add_field_5feae(void *writer, uint8_t value); /* vendor // 0x0005feae */
/* TinyCBOR wrapper: init a reader over (src_a,src_b), require tag 0x60, copy the
 * byte-string into dst bounded by max; returns the length or 0xffffffff. */
extern uint32_t findmy_cbor_get_bstr_58cd8(void *dst, uint32_t max, uint32_t src_a, uint32_t src_b);  /* vendor // 0x00058cd8 */

/* --- VanMoof callees not in this batch (forward decls; carved/declared later) --- */
/* Build a framed FMNA transport message into buf (type/seq/chan/frame_id header,
 * CRC-16, copied payload); returns the framed length or -1 on overflow. // 0x00058b12 */
int findmy_build_message(void *buf, uint32_t seq, uint8_t type, uint8_t chan,
                         uint16_t frame_id, const void *payload, int payload_len);
/* SPI-bridge consumer thread (0x0003ef1c) — DEFERRED, not yet reconstructed; see
 * docs/progress.md (vendor k_poll/k_pipe dataflow unresolved). */

/* --- prototypes (carved batch 4) --- */
void     findmy_handle_conn_rx(uint32_t conn, const uint16_t *buf, uint32_t len);  /* 0x0003c6f4 */
uint32_t findmy_conn_event_handler(const uint8_t *evt);                            /* 0x0003cabc */
void     findmy_forward_peer_payload(const uint8_t *rec);                          /* 0x0003cdcc */
void     findmy_send_status_report(void);                                          /* 0x0003ce20 */
void     findmy_reset_conn_slots(void);                                            /* 0x0003d134 */
uint32_t findmy_msg_enqueue(uint32_t tag, uint8_t kind, const void *src, uint16_t len);  /* 0x0003d160 */
void     findmy_store_provisioning_token(uint32_t a0, uint32_t a1, uint32_t a2,
                                         uint32_t src_a, uint32_t src_b);          /* 0x0003d29c */
/* spi_bridge_unlock (0x0003ef10), findmy_alloc_work_item (0x0003d220),
 * findmy_match_provisioning_topic (0x0003d234) and findmy_send_state_report
 * (0x0003d47c) are already declared above. */

#endif /* BLE_H */
