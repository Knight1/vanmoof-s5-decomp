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
extern void ble_event_post(void);  /* vendor // 0x00040d98 */
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
void *auth_alloc_disconnect_event(void);  /* 0x0003e164 */
void *auth_alloc_connection_event(void);  /* 0x0003e178 */
void *auth_alloc_event_0x14(void);  /* 0x0003e574 */
void auth_format_ble_address(const uint8_t *addr, char *out);  /* 0x0003e2d8 */
void auth_init_connection_slots(void);  /* 0x0003e45c */
void auth_send_disconnect_reason(uint32_t conn_handle, uint8_t flag, uint32_t reason);  /* 0x0003e4b0 */

#endif /* BLE_H */
