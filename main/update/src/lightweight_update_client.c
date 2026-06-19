#include "update_common.h"

/* ===== module-local framework model (externs + structs) ===== */
typedef void (*od_fn_t)(void);  /* generic OD reply callback (cast at call site) */
/* Modelled std::string (libstdc++ SSO layout). Concrete so it can be embedded

 * by value. ptr==inl means small-string-optimized (inline storage). */

typedef struct str_t {

    char    *ptr;        /* +0x00  data pointer (== inl when SSO)        */

    unsigned long len;   /* +0x08  length                               */

    char     inl[16];    /* +0x10  inline buffer / capacity union        */

} str_t;



/* Config blob passed to the ctor (param_4): { a, b, timeout_ms, ... }. */

typedef struct client_cfg_t {

    void         *a;          /* +0x00  param_4[0] */

    void         *b;          /* +0x08  param_4[1] */

    unsigned int  timeout_ms; /* +0x10  param_4[2] -> client off 0x48 */

} client_cfg_t;



/* LightweightUpdateClient — offsets are byte offsets into the OEM object.

 * Only the fields this TU touches are named; the rest is padding/opaque. */

typedef struct LightweightUpdateClient {

    void   *vptr;             /* +0x000  &PTR_FUN_0019d120                      */

    void   *od_owner;         /* +0x008  (param_3)                             */

    void   *transport;        /* +0x010  OD/CAN transport (param_2)            */

    void   *ops_begin;        /* +0x018  registered-op vector begin           */

    void   *ops_end;          /* +0x020  vector end                           */

    void   *ops_cap;          /* +0x028  vector cap                           */

    uint8_t node_id;          /* +0x030  resolved CAN node id (off 6 in u64 idx)*/

    uint8_t _pad031[7];

    void   *cfg_a;            /* +0x038                                       */

    void   *cfg_b;            /* +0x040                                       */

    unsigned int timeout_ms;  /* +0x048  overall wait timeout (ms)             */

    unsigned int _pad04c;

    const char  *update_path; /* +0x050  update file path / node name (param_5) */

    uint8_t some_flag;        /* +0x058  (param_6)                            */

    uint8_t _pad059[7];

    void   *pending_action;   /* +0x060  live OD action handle (sz 0x58)       */

    /* +0x068 .. mutex (pthread_mutex_t), +0x098 cond (pthread_cond_t),

       +0x098 condvar object built at param_1[0x13] */

    char    mutex[0x30];      /* +0x068  pthread_mutex_t                       */

    char    cond[0x30];       /* +0x098  pthread_cond_t / condition_variable    */

    char    update_failed;    /* +0x0c8  latched result code (0=ok)           */

    char    result_latched;   /* +0x0c9  result recorded flag                 */

    uint8_t _pad0ca[2];

    int     state;            /* +0x0cc  flash state machine (1..4,6)         */

    str_t   result_msg;       /* +0x0d0  reason string (off 0xd0 ptr,0xe0 inl) */

    uint8_t result_msg_set;   /* +0x0f0  result_msg constructed flag          */

    uint8_t _pad0f1[7];

    unsigned long page_width; /* +0x0f8  page width  (from flash-size args[0]) */

    unsigned long page_count; /* +0x100  page count  (from flash-size args[1]) */

    short   page_index;       /* +0x108  current page index                   */

    short   page_remaining;   /* +0x10a  pages remaining counter (param_1[0x21])*/

    int     expected_crc;     /* +0x10c  expected CRC for current step         */

    unsigned int page_retry;  /* +0x110  per-page CRC retry count (param_1[0x22])*/

    /* +0x118 image_begin, +0x120 image_end, +0x128 image_cap (file buffer) */

    void   *image_begin;      /* +0x118                                       */

    void   *image_end;        /* +0x120                                       */

    void   *image_cap;        /* +0x128                                       */

    /* +0x118 region aliased below in vector terms used by CRC assembly: */

    unsigned long page_width_full; /* +0x0f8 alias (param_1[0x1f]) bytes/page  */

    unsigned long total_pages;     /* +0x100 alias (param_1[0x20]) total pages */

    void   *page_buf_begin;   /* +0x118  (param_1[0x23]) staged page bytes begin*/

    void   *page_buf_end;     /* +0x120  (param_1[0x24]) staged page bytes end  */

    void   *writecb_action;   /* +0x060  alias used in crc cb (param_1[0xc])   */

    uint8_t result_msg_inline_used; /* +0x0f0 alias (param_1[0x1e] in dtor)    */

} LightweightUpdateClient;

/* ---- vtable / typeinfo placeholders (read-only addresses) ---- */
extern void *PTR_FUN_0019d120;            /* LightweightUpdateClient vtable */

/* ---- operator new/delete / EH (modelled toolchain runtime) ---- */
extern void *operator_new(unsigned long sz);
extern void  operator_delete(void *p, ...);   /* C++ op delete (sized + unsized) */
__attribute__((noreturn)) void throw_invalid_argument(const char *what);
__attribute__((noreturn)) void throw_vm_call_error(const char *op_name); /* runtime_error "Failed on a VM call '<op>': <rc>" */

/* ---- modelled std::string (str_t is a concrete type, see structs[]) ---- */
void   str_init_cstr(str_t *s, const char *cstr);
void   str_init_copy(str_t *dst, const str_t *src);
void   str_init_fmt(str_t *s, const char *fmt, ...);
void   str_init_from_functor(str_t *s, void *functor);  /* payload builder result */
void   str_assign(str_t *dst, const str_t *src);
void   str_append_cstr(str_t *s, const char *cstr);
const char *str_data(const str_t *s);
void   str_destroy(str_t *s);

/* ---- node-name -> CAN node id resolver  OEM 0x0012fbc0 ----
 * Verbatim map (subset): CORE_SERVICES=0, HEARTBEAT=8, POWER=0x82, RIDE=0x84,
 * LOGGING=0x87, UX=0x88, UPDATE=0x8a, MQTT_OD_BRIDGE=0x8b, MQTT_FTP=0x8c,
 * MOTOR_CONTROL=0x8d, IMX8_BRIDGE=0x8f, BLE=0x90, MODEM=0x91, MOTOR_SENSOR=0xa1,
 * POWER_PEDAL=0xa2, POWER_CONTROL=0xa3, BATTERY_PRIMARY=0xa4, BATTERY_SECONDARY=0xa5,
 * CHARGER=0xa7, USER_ECU=0xc0, ELOCK=0xc1, ESHIFTER=0xc2, REARLIGHT=0xc3,
 * FRONTLIGHT=0xc4, PHONE=0xe0, BACKOFFICE=0xe2, PING=0xfb, DUMMY=0xfc,
 * BATTERY_TEST=0xfd, RASPBERRY=0xfe, TEST/unknown=0xff (*ok=0 if unknown). */
extern uint8_t vm_resolve_node_id(const char *name, uint8_t *ok);

/* ---- OD register helpers (bind an OD entry id to node + callback) ----
 * Reproduce the OEM OD entry ids verbatim. The 0x200_____ ids carry the
 * extended/segment flag; the low half is the OD index. */
extern char od_register_flash    (void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013d940 id 0x8a81    sz4 */
extern char od_register_erasepage(void *od, uint8_t node);                      /* 0x0013d9f0 id 0x2008a82 sz4 */
extern char od_register_write    (void *od, uint8_t node);                      /* 0x0013daa0 id 0x2008a83 sz8 */
extern char od_register_crc      (void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013db50 id 0x8a84    sz4+sub2 */
extern char od_register_updatereq(void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013dc20 id 0x8a85    sz1+sub4 */
extern char od_register_reboot   (void *od, uint8_t node);                      /* 0x0013dcf0 id 0x2008a86 sz0 */

/* ---- OD action (send named action, await reply within timeout) ---- */
extern void *od_action_new(void *od, str_t *name, str_t *payload, int reply_kind, int *timeout_ms_ptr); /* FUN_00152020, obj sz 0x58 */
extern char  od_action_dispatched(void *action);   /* FUN_001521c0 */
extern void  od_action_destroy(void *action);       /* FUN_00152150 */
extern void  od_op_destroy(void *op);               /* FUN_0016b190, op stride 0x40 */

/* ---- OD reboot request  OEM 0x0013e510 (query FUN_0013e460 + commit FUN_0013e4b0) ---- */
extern char od_request_reboot(void *od, uint8_t node, int kind);

/* ---- page / full-image CRC  OEM 0x0016ee70 ---- */
extern uint32_t lw_page_crc(const void *data, size_t len);

/* ---- update-file reader  OEM 0x00123b30 (fills file_buf_t) ---- */
typedef struct { void *begin; void *end; void *cap; void *begin_orig; } file_buf_t;
extern void read_update_file(file_buf_t *out, const char *path);

/* ---- payload-builder functor manager fns (std::function-style) ----
 * Selected by the action being sent; each emits the OD payload bytes.
 * Modelled as opaque tokens passed to lw_start_action. */
enum { PAYLOAD_FLASH_SIZE_REQ, PAYLOAD_ADVANCEPAGE, PAYLOAD_UPDATE_REQUEST, PAYLOAD_FULL_CRC };

/* ---- interface sub-object dtor (IUpdateClient base)  OEM 0x00131ab0 ---- */
extern void LightweightUpdateClient_iface_dtor(LightweightUpdateClient *c);

/* ---- threading primitives (modelled) ---- */
extern void cv_init(void *cv);
extern void cv_destroy(void *cv);
extern void cv_notify_all(void *cv);
extern int  pthread_mutex_lock(void *m);
extern int  pthread_mutex_unlock(void *m);
extern long steady_now_ns(void);
extern long system_now_ns(void);
extern int  cond_timedwait_ns(void *cv, void *m, long abs_ns);
/* =========================================================== */

/*
 * devices/main/update/src/lightweight_update_client.cpp
 *
 * LightweightUpdateClient — the trimmed CAN page-flash OTA client used for the
 * "lib" targets (e.g. MOTOR_CONTROL).  Unlike ThirdPartyUpdateClient it does no
 * charger validation; it just drives a node through the vm Object-Dictionary
 * page-flash protocol:
 *
 *   PerformUpdate         -> sends "Initial flash size request" (OD "flash" op),
 *                            then blocks on a condvar until terminal (state 6).
 *   flash size response   -> lightweight_flash_size_cb  (OD op id 0x8a81)
 *   per-page CRC response  -> lightweight_crc_cb         (OD op id 0x8a84)
 *   update-request reply   -> lightweight_update_req_cb  (OD op id 0x8a85)
 *   ErasePage / write / reboot ops registered but driven from the callbacks.
 *
 * The flash state machine (client->state at off 0xcc):
 *   1 = waiting for flash-size info
 *   2 = streaming pages, awaiting per-page CRC
 *   3 = awaiting full-image CRC
 *   4 = awaiting update-request (apply) reply
 *   6 = terminal (result latched)
 *
 * OEM ABI is preserved; framework objects (std::string, OD transport, condvar,
 * mutex) are modelled via the externs/structs supplied alongside this TU.
 */
/* ---- modelled framework symbols (provided via externs[]) ---- */

/* Resolve a node name ("MOTOR_CONTROL", ...) to its CAN node id.  *ok set to 1
 * on a known name. OEM 0x0012fbc0 — verbatim node-id map preserved in externs. */
extern uint8_t vm_resolve_node_id(const char *name, uint8_t *ok);

/* OD register-callback helpers.  Each binds (transport, node_id, fn, ctx) to an
 * OD entry id; returns 0 on success, non-zero error. OEM addresses noted. */
extern char od_register_flash    (void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013d940 id 0x8a81   */
extern char od_register_erasepage(void *od, uint8_t node);                      /* 0x0013d9f0 id 0x2008a82 */
extern char od_register_write    (void *od, uint8_t node);                      /* 0x0013daa0 id 0x2008a83 */
extern char od_register_crc      (void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013db50 id 0x8a84   */
extern char od_register_updatereq(void *od, uint8_t node, od_fn_t fn, void *ctx); /* 0x0013dc20 id 0x8a85   */
extern char od_register_reboot   (void *od, uint8_t node);                      /* 0x0013dcf0 id 0x2008a86 */

/* OD "send named action with payload, expect reply within timeout_ms" handle.
 * FUN_00152020 builds it; FUN_001521c0 returns whether the call was dispatched. */
extern void *od_action_new(void *od, str_t *name, str_t *payload, int reply_kind,
                           int *timeout_ms_ptr);          /* sizeof 0x58 */
extern char  od_action_dispatched(void *action);          /* FUN_001521c0 */
extern void  od_action_destroy(void *action);             /* FUN_00152150 */

/* Issue an OD reboot request to the node. OEM 0x0013e510 (FUN_0013e460 query +
 * FUN_0013e4b0 commit); returns 0 on success. */
extern char od_request_reboot(void *od, uint8_t node, int kind);

/* CRC over a byte buffer used as the page / full-image checksum. OEM 0x0016ee70. */
extern uint32_t lw_page_crc(const void *data, size_t len);

extern str_t *str_assign_cstr(str_t *dst, const char *src);

/* condvar / mutex modelled opaque (off 0x68 mutex, 0x98 cond, 0x13 condvar) */
extern int  pthread_mutex_lock(void *m);
extern int  pthread_mutex_unlock(void *m);
extern void cv_notify_all(void *cv);
extern long steady_now_ns(void);
extern long system_now_ns(void);
extern int  cond_timedwait_ns(void *cv, void *m, long abs_ns);


/* ==========================================================================
 * lightweight_client_set_update_result   OEM 0x00131850
 *
 * Latch the terminal result.  Once state == 6 this is a no-op.  Stores the
 * reason string, sets state 6, then under the mutex records success/failure
 * exactly once and wakes PerformUpdate's wait loop via the condvar.
 * ========================================================================== */
void lightweight_client_set_update_result(LightweightUpdateClient *c,
                                          str_t *reason, char failed)
{
    if (c->state == 6)               /* already terminal */
        return;

    if (!c->result_msg_set) {
        str_init_copy(&c->result_msg, reason);
        c->result_msg_set = 1;
    } else {
        str_assign(&c->result_msg, reason);
    }
    c->state = 6;

    /* guard mirrors PTR___pthread_key_create != 0 (threads present) */
    pthread_mutex_lock(&c->mutex);

    if (!c->result_latched) {
        if (failed == 0) {
            common_logf("devices/main/update/src/lightweight_update_client.cpp",
                        0xd9, LOG_INFO, "Update succeeded");
        } else {
            common_logf("devices/main/update/src/lightweight_update_client.cpp",
                        0xdb, LOG_ERR, "Update failed. Reason: %s",
                        str_data(reason));
        }
        c->update_failed   = failed;   /* off 0xc8 */
        c->result_latched  = 1;        /* off 0xc9 */
    }

    pthread_mutex_unlock(&c->mutex);
    cv_notify_all(&c->cond);
}


/* ==========================================================================
 * StartAction (page-flash protocol primitive)        OEM 0x00132250
 *
 * Cancel any pending OD action, then send a named OD action (`name`) with a
 * payload built by `payload_fn`, expecting a reply within 2000 ms.  If the call
 * could not even be dispatched, fail the update immediately; otherwise stash
 * the live action handle at off 0x60 so the next response can supersede it.
 *
 * param order matches the OEM: (this, std::string* name, functor* payload).
 * ========================================================================== */
void lw_start_action(LightweightUpdateClient *c, str_t *name, void *payload_fn)
{
    void *prev = c->pending_action;          /* off 0x60 */
    c->pending_action = 0;
    if (prev) {
        od_action_destroy(prev);
        operator_delete(prev, 0x58);
    }

    {
        str_t nm, pl;
        int timeout_ms = 2000;               /* OEM local_a0 = 2000 */
        void *action;

        str_init_copy(&nm, name);
        /* payload functor (FUN_00132040 manager) produces the page bytes */
        str_init_from_functor(&pl, payload_fn);

        action = od_action_new(c->transport /*off 8*/, &nm, &pl,
                               /*reply_kind*/3, &timeout_ms);

        str_destroy(&pl);
        str_destroy(&nm);

        if (!od_action_dispatched(action)) {
            lightweight_client_set_update_result(c, name, (char)0xff);
            od_action_destroy(action);
            operator_delete(action, 0x58);
        } else {
            void *old = c->pending_action;
            c->pending_action = action;
            if (old) {
                od_action_destroy(old);
                operator_delete(old, 0x58);
            }
        }
    }
}


/* ==========================================================================
 * AdvancePage                                         OEM 0x00132550
 *
 * Bump the page index (off 0x108) and ask the node to advance to the next page
 * via the "AdvancePage" OD action.  Re-armed after each per-page CRC match.
 * ========================================================================== */
void lw_advance_page(LightweightUpdateClient *c)
{
    str_t name;
    str_init_cstr(&name, "AdvancePage");     /* s_AdvancePage_00173fe8, len 0xb */
    c->page_index++;                          /* off 0x108 */
    lw_start_action(c, &name, /*payload functor*/ (void *)PAYLOAD_ADVANCEPAGE);
    str_destroy(&name);
}


/* ==========================================================================
 * flash-size response callback                        OEM 0x00132670
 *
 * OD id 0x8a81.  param_3 -> two uint16: [0]=page_width(0xf8), [1]=page_count
 * (0x100).  Only valid in state 1.  Verifies the device's reported flash size
 * (page_width * page_count) covers (image_end - image_begin + 4); if so go to
 * state 2 and send the first page (AdvancePage).  Otherwise fail.
 * ========================================================================== */
int lightweight_flash_size_cb(LightweightUpdateClient *c, void *od, uint16_t *args)
{
    void *prev = c->pending_action;          /* off 0x60 */
    c->pending_action = 0;
    if (prev) { od_action_destroy(prev); operator_delete(prev, 0x58); }
    (void)od;

    if (c->state != 1) {
        str_t msg;
        str_init_cstr(&msg, "Flash size info arrived at an unexpected moment"); /* 0x173ff8 */
        lightweight_client_set_update_result(c, &msg, (char)0xff);
        str_destroy(&msg);
        return 0;
    }

    {
        uint16_t page_width = args[0];
        uint16_t page_count = args[1];
        unsigned long need;

        c->page_index  = 0;                  /* off 0x108 */
        c->page_width  = page_width;         /* off 0xf8  */
        c->page_count  = page_count;         /* off 0x100 */

        /* image_end (0x120) - image_begin (0x118) + 4 = bytes that must fit */
        need = (unsigned long)((uint8_t*)c->image_end - (uint8_t*)c->image_begin) + 4;

        if (need <= (unsigned long)page_width * (unsigned long)page_count) {
            c->state = 2;
            lw_advance_page(c);              /* stream first page */
        } else {
            /* "Reported flash size (%lu) is not big enough for the update (%lu)" */
            str_t reported, capacity, msg;
            str_init_fmt(&reported, "%lu", (unsigned long)page_width * page_count);
            str_init_fmt(&capacity, "%lu", need);
            str_init_cstr(&msg, "Reported flash size (");           /* 0x174030 */
            str_append_cstr(&msg, str_data(&reported));
            str_append_cstr(&msg, ") is not big enough for the update ("); /* 0x174048 */
            str_append_cstr(&msg, str_data(&capacity));
            str_append_cstr(&msg, ")");                              /* 0x1702f8 */
            lightweight_client_set_update_result(c, &msg, (char)0xff);
            str_destroy(&msg);
            str_destroy(&capacity);
            str_destroy(&reported);
        }
    }
    return 0;
}


/* ==========================================================================
 * per-page / full-image CRC response callback         OEM 0x00132e80
 *
 * OD id 0x8a84.  param_3 -> int received CRC.  Handles three states:
 *
 *  state 2 (page CRC):
 *      match (crc == expected@0x10c)      -> reset retry, advance page index.
 *      mismatch                           -> page_remaining--, retry++; after
 *                                            3 failed attempts -> "CRC check of
 *                                            a page failed 3 times" (fail).
 *      when all pages done (page_remaining == total) -> assemble full image
 *      (0xff-padded buffer of page_width*page_count, memcpy the file bytes),
 *      compute "CRC over full image", go to state 3, request full-image CRC.
 *
 *  state 3 (full-image CRC):
 *      match -> state 4, send "Update request" (apply).
 *      mismatch -> "CRC check of the entire image failed" (fail).
 *
 *  state 6: ignore.  any other state: "CRC received at an unexpected time".
 * ========================================================================== */
int lightweight_crc_cb(LightweightUpdateClient *c, void *od, int *p_crc)
{
    void *prev = c->writecb_action;          /* off 0xc * 8 = 0x60-region (param_1[0xc]) */
    c->writecb_action = 0;
    if (prev) { od_action_destroy(prev); operator_delete(prev, 0x58); }
    (void)od;

    if (c->state == 3) {
        if (*p_crc == c->expected_crc) {     /* off 0x10c */
            uint32_t full_crc;
            c->state = 4;
            full_crc = lw_page_crc(c->page_buf_begin /*0x23*8*/,
                                   (size_t)((uint8_t*)c->page_buf_end - (uint8_t*)c->page_buf_begin));
            c->expected_crc = full_crc;
            {
                str_t name;
                str_init_cstr(&name, "Update request");   /* 0x174118, len 0xe */
                lw_start_action(c, &name, (void *)PAYLOAD_UPDATE_REQUEST);
                str_destroy(&name);
            }
        } else {
            str_t msg;
            str_init_cstr(&msg, "CRC check of the entire image failed"); /* 0x1740f0 */
            lightweight_client_set_update_result(c, &msg, (char)0xff);
            str_destroy(&msg);
        }
        return 0;
    }

    if (c->state == 6)
        return 0;

    if (c->state == 2) {
        uint16_t remaining = c->page_remaining;          /* off 0x108-region (param_1[0x21]) */

        if (*p_crc == c->expected_crc) {
            c->page_retry = 0;                           /* off 0x110 (param_1[0x22]) */
        } else {
            unsigned r = c->page_retry;
            remaining = (uint16_t)(remaining - 1);
            c->page_remaining = remaining;
            c->page_retry = r + 1;
            if (r + 1 > 2) {                             /* 3rd failure */
                str_t msg;
                str_init_cstr(&msg, "CRC check of a page failed 3 times"); /* 0x1740b0 */
                lightweight_client_set_update_result(c, &msg, (char)0xff);
                str_destroy(&msg);
                return 0;
            }
        }

        if ((uint64_t)c->total_pages == (uint64_t)remaining) {  /* off param_1[0x20] */
            /* all pages acknowledged -> assemble full image, ask for full CRC */
            size_t page_w = c->page_width_full;          /* param_1[0x1f] */
            size_t n      = (size_t)remaining * page_w;
            void  *image  = 0;
            uint32_t full_crc;

            c->state = 3;
            if (n) {
                image = operator_new(n);
                memset(image, 0xff, n);
            }
            memcpy(image, c->page_buf_begin,
                   (size_t)((uint8_t*)c->page_buf_end - (uint8_t*)c->page_buf_begin));
            full_crc = lw_page_crc(image, n);
            c->expected_crc = full_crc;

            {
                str_t name;
                str_init_cstr(&name, "CRC over full image");   /* 0x1740d8, len 0x13 */
                lw_start_action(c, &name, (void *)PAYLOAD_FULL_CRC);
                str_destroy(&name);
            }
            operator_delete(image);
        } else {
            lw_advance_page(c);              /* send the next page */
        }
        return 0;
    }

    /* unexpected state */
    {
        str_t msg;
        str_init_cstr(&msg, "CRC received at an unexpected time"); /* 0x174128 */
        lightweight_client_set_update_result(c, &msg, (char)0xff);
        str_destroy(&msg);
    }
    return 0;
}


/* ==========================================================================
 * update-request (apply) reply callback               OEM 0x00131c60
 *
 * OD id 0x8a85.  param_3 -> one byte: 0 = device accepted, non-zero = refused.
 * Only valid in state 4.
 *   accepted  -> issue OD reboot (od_request_reboot, FUN_0013e510); on success
 *                latch "success" (failed=0), else "Failed to request reboot".
 *   refused   -> "Device refused to apply update".
 *   wrong state -> "Response to update request arrived at an unexpected time".
 * ========================================================================== */
int lightweight_update_req_cb(LightweightUpdateClient *c, void *od, char *p_reply)
{
    void *prev = c->pending_action;          /* off 0x60 */
    c->pending_action = 0;
    if (prev) { od_action_destroy(prev); operator_delete(prev, 0x58); }
    (void)od;

    if (c->state == 4) {
        if (*p_reply == 0) {
            /* device accepted -> request a reboot to apply */
            char err = od_request_reboot(c->transport, c->node_id, 0);
            if (err == 0) {
                str_t msg;
                str_init_cstr(&msg, "success");                    /* 0x173f70 */
                lightweight_client_set_update_result(c, &msg, 0);  /* failed=0 */
                str_destroy(&msg);
            } else {
                str_t msg;
                str_init_cstr(&msg, "Failed to request reboot");   /* 0x173f50 */
                lightweight_client_set_update_result(c, &msg, (char)0xff);
                str_destroy(&msg);
            }
        } else {
            str_t msg;
            str_init_cstr(&msg, "Device refused to apply update"); /* 0x173f30 */
            lightweight_client_set_update_result(c, &msg, (char)0xff);
            str_destroy(&msg);
        }
    } else {
        str_t msg;
        str_init_cstr(&msg,
            "Response to update request arrived at an unexpected time"); /* 0x173ef0 */
        lightweight_client_set_update_result(c, &msg, (char)0xff);
        str_destroy(&msg);
    }
    return 0;
}


/* ==========================================================================
 * PerformUpdate                                        OEM 0x00132b80
 *
 * Read the update file into the image buffer (image_begin 0x118 / image_end
 * 0x120 / cap 0x128 via FUN_00123b30), send the "Initial flash size request"
 * (the "flash" OD action), then block under the mutex on the condvar until the
 * result is latched (result_latched at 0xc9) or the overall timeout (off 0x48,
 * ms) elapses — on timeout, latch a 0x1ff failure code.  Finally cancel any
 * pending OD action.
 * ========================================================================== */
void LightweightUpdateClient_PerformUpdate(LightweightUpdateClient *c)
{
    /* load update file bytes into [image_begin, image_end), capacity at 0x128 */
    file_buf_t fb;
    read_update_file(&fb, c->update_path /*off 0x50*/);
    {
        void *old = c->image_begin;
        c->image_begin = fb.begin;           /* off 0x118 */
        c->image_end   = fb.end;             /* off 0x120 */
        c->image_cap   = fb.cap;             /* off 0x128 */
        if (old) operator_delete(old);
        if (fb.begin_orig) operator_delete(fb.begin_orig);
    }

    /* kick off: OD "flash" action carrying "Initial flash size request" */
    {
        str_t name;
        str_init_cstr(&name, "Initial flash size request");  /* 0x174070, len 0x1a */
        lw_start_action(c, &name, (void *)PAYLOAD_FLASH_SIZE_REQ);
        str_destroy(&name);
    }

    /* wait for terminal state or timeout (c->timeout_ms at off 0x48) */
    pthread_mutex_lock(&c->mutex);
    {
        long deadline = steady_now_ns() + (long)c->timeout_ms * 1000;
        while (!c->result_latched) {        /* off 0xc9 */
            long want_sys = (deadline - steady_now_ns()) + system_now_ns();
            cond_timedwait_ns(&c->cond, &c->mutex, want_sys);
            if (system_now_ns() >= want_sys && steady_now_ns() >= deadline) {
                if (!c->result_latched)
                    c->update_failed = (char)0x1ff;  /* off 0xc8/0xc9 word = 0x1ff */
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);

    {
        void *pending = c->pending_action;   /* off 0x60 */
        if (pending) {
            c->pending_action = 0;
            od_action_destroy(pending);
            operator_delete(pending, 0x58);
        }
    }
}


/* ==========================================================================
 * LightweightUpdateClient ctor                         OEM 0x00133370
 *
 * Stores transport (off 8), OD owner (off 0x10), the update path (off 0x48..),
 * timeout (off 0x48) and flags, resolves the destination node id by name
 * (vm_resolve_node_id, FUN_0012fbc0) into off 6 — throwing std::invalid_argument
 * if unknown — then registers the six OD entries against (transport, node_id):
 *
 *      "flash"     id 0x8a81   -> lightweight_flash_size_cb
 *      "ErasePage" id 0x2008a82 (no callback)
 *      "write"     id 0x2008a83 (no callback)
 *      "crc"       id 0x8a84   -> lightweight_crc_cb
 *      "update"    id 0x8a85   -> lightweight_update_req_cb
 *      "reboot"    id 0x2008a86 (no callback)
 *
 * Each failed registration throws  std::runtime_error("Failed on a VM call '<op>': <rc>").
 * ========================================================================== */
void LightweightUpdateClient_ctor(LightweightUpdateClient *c,
                                  void *transport, void *od_owner,
                                  client_cfg_t *cfg, const char *node_name,
                                  uint8_t flags)
{
    char ok = 0;

    c->vptr           = &PTR_FUN_0019d120;
    c->od_owner       = od_owner;            /* off 8  (param_3) */
    c->transport      = transport;           /* off 0x10 (param_2) */
    c->ops_begin = c->ops_end = c->ops_cap = 0;
    c->timeout_ms     = cfg->timeout_ms;     /* off 0x48 (param_4[2]) */
    c->update_path    = node_name;           /* off 0x50 (param_5) */
    c->some_flag      = flags;               /* off 0x58 (param_6) */
    c->cfg_a          = cfg->a;              /* off 0x38 (param_4[0]) */
    c->cfg_b          = cfg->b;              /* off 0x40 (param_4[1]) */
    cv_init(&c->cond);
    c->result_latched = 0;
    c->state          = 0;                   /* off 0xcc */
    c->result_msg_set = 0;
    /* page buffers / counters zeroed */

    c->node_id = vm_resolve_node_id(node_name, (uint8_t *)&ok);   /* off 6 */
    if (!ok) {
        common_logf("devices/main/update/src/lightweight_update_client.cpp",
                    0x49, LOG_ERR, "Unable to resolve address for %s", node_name);
        throw_invalid_argument("Unable to resolve address.");
    }

    /* Register the six OD entries; any non-zero rc -> runtime_error. */
    if (od_register_flash(transport, c->node_id,
                          (od_fn_t)lightweight_flash_size_cb, c))
        throw_vm_call_error("flash");
    if (od_register_erasepage(transport, c->node_id))
        throw_vm_call_error("ErasePage");
    if (od_register_write(transport, c->node_id))
        throw_vm_call_error("write");
    if (od_register_crc(transport, c->node_id,
                        (od_fn_t)lightweight_crc_cb, c))
        throw_vm_call_error("crc");
    if (od_register_updatereq(transport, c->node_id,
                              (od_fn_t)lightweight_update_req_cb, c))
        throw_vm_call_error("update");
    if (od_register_reboot(transport, c->node_id))
        throw_vm_call_error("reboot");
}


/* ==========================================================================
 * LightweightUpdateClient dtor                         OEM 0x00131b70
 *
 * Restores the vtable, releases the page buffer (off 0x23*8), the optional
 * inline reason buffer (off 0x1a), the condvar, any pending OD action (off
 * 0x60-region), and the registered-ops vector.
 * ========================================================================== */
void LightweightUpdateClient_dtor(LightweightUpdateClient *c)
{
    c->vptr = &PTR_FUN_0019d120;
    LightweightUpdateClient_iface_dtor(c);   /* FUN_00131ab0 */

    if (c->page_buf_begin)
        operator_delete(c->page_buf_begin);

    if (c->result_msg_inline_used) {
        c->result_msg_inline_used = 0;
        if (c->result_msg.ptr != c->result_msg.inl)
            operator_delete(c->result_msg.ptr);
    }

    cv_destroy(&c->cond);

    if (c->pending_action) {
        od_action_destroy(c->pending_action);
        operator_delete(c->pending_action, 0x58);
    }

    {
        void *p = c->ops_begin;
        void *e = c->ops_end;
        if (p != e) {
            do { od_op_destroy(p); p = (char *)p + 0x40; } while (p != e);
        }
        if (c->ops_begin) operator_delete(c->ops_begin);
    }
}