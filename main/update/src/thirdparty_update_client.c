#include "update_common.h"

/* ============ module-local framework model (externs + structs) ============ */
typedef struct str { char data[128]; } str_t;  /* concrete str model */
/*
 * TpClient - the third-party (battery/charger) update client object.
 * Field comments are the OEM byte offsets from the decompiled struct (the
 * 'update' image). Only the fields touched by this TU are modelled; the rest
 * of the C++ object (IUpdateClient base, the VM-call vector, the mutex/condvar
 * storage) is represented by opaque byte padding so the offsets line up.
 *
 * Per-vendor timing words [9..12] live at 0x48..0x60; mode at 0x68. The FSM
 * state is the 32-bit int at 0xf4; the result string is the std::string at
 * 0xf8 (SSO buffer 0x108, set-flag 0x118). Page geometry occupies 0x120..0x138.
 */
typedef struct TpClient {
    const void *vptr;                 /* 0x00  C++ vtable (PTR_FUN_0019d1*)     */
    uint8_t  _pad08[0x08];            /* 0x08  transport/log handle word        */
    void    *transport;              /* 0x08  arg to tp_op_create (alias)      */
    void    *od_handle;              /* 0x10  VM OD handle                     */
    uint8_t  _pad18[0x18];           /* 0x18                                   */
    uint8_t  od_node;                /* 0x30  OD node id                       */
    uint8_t  _pad31[0x07];           /* 0x31                                   */
    void    *update_file;            /* 0x38  update-file object               */
    uint8_t  _pad40[0x08];           /* 0x40                                   */
    int64_t  step_delay_us;          /* 0x48  [9]  erase/step settle (us)      */
    int64_t  inter_word_delay_us;    /* 0x50  [10] between page words (us)     */
    int64_t  charger_wait_unit;      /* 0x58  [11] charger-validate wait unit  */
    int64_t  result_wait_unit;       /* 0x60  [12] flash-size req timeout unit */
    int32_t  mode;                   /* 0x68  [0xd] 1=skip lazy img CRC, 0=lazy */
    uint8_t  _pad6c[0x04];           /* 0x6c                                   */
    uint8_t *file_begin;             /* 0x70  update image buffer begin        */
    uint8_t *file_end;               /* 0x78  update image buffer end          */
    uint8_t *file_cap;               /* 0x80  update image buffer capacity     */
    void    *pending_op;             /* 0x88  in-flight queued CAN op (0x58 B) */
    uint8_t  _pad90[0x60];           /* 0x90  result mutex + condvar @0xc0      */
    char     result_code;            /* 0xf0  latched result code (low byte)   */
    uint8_t  result_flag;            /* 0xf1  result latched flag              */
    uint8_t  _padf2[0x02];           /* 0xf2                                   */
    int32_t  state;                  /* 0xf4  FSM state (2/3/4/5/7)             */
    str_t    reason;                 /* 0xf8  result-reason std::string        */
    uint8_t  _pad100[0x10];          /* 0x100 SSO buffer (0x108)               */
    uint8_t  reason_set;             /* 0x118 reason-string initialized flag   */
    uint8_t  _pad119[0x07];          /* 0x119                                  */
    uint64_t page_size;              /* 0x120 [0x24] flash page size (bytes)   */
    uint64_t page_count;             /* 0x128 [0x25] flash page count          */
    uint16_t current_page;           /* 0x130 [0x26] current page index        */
    uint8_t  _pad132[0x02];          /* 0x132                                  */
    uint32_t expected_crc;           /* 0x134 expected/computed CRC32          */
    uint32_t retry_count;            /* 0x138 [0x27] per-page CRC retry count  */
    uint8_t  charger_type_ok;        /* 0x13c (Liteon) charger validated flag  */
    uint8_t  _pad13d[0x03];          /* 0x13d                                  */
    uint8_t  _pad140[0x30];          /* 0x140 charger-validate condvar         */
} TpClient;

/* --- update_common.h is assumed; these are everything ELSE the TU needs --- */

/* std::string model (the str_* set used here, beyond update_common.h's). The
   real type is libstdc++ std::__cxx11::string; modelled as an opaque handle. */
void   str_init(str_t *s, const char *cstr);
void   str_free(str_t *s);
void   str_append(str_t *s, const char *cstr);
void   str_assign(str_t *dst, const str_t *src);
const char *str_cstr(const str_t *s);
bool   str_equals(const str_t *s, const str_t *other);
int    tp_snprintf(char *buf, size_t cap, const char *fmt, ...);

/* C++ operator new/delete (OEM operator_new / operator_delete). */
void  *operator_new(size_t n);
void   operator_delete(void *p);
void   operator_delete_sz(void *p, size_t n);     /* sized delete, e.g. 0x58 */

/* Pending-op (queued CAN command) lifecycle - OEM FUN_00152150/FUN_00152020/
   FUN_001521c0. tp_op_create allocates+inits the op object (0x58 bytes);
   tp_op_enqueue submits it (returns false on failure); tp_op_destroy tears
   down its internals (the operator_delete_sz frees the 0x58 block). */
void  *tp_op_create(void *transport, const str_t *name, void *ctx, int64_t timeout_ms);
bool   tp_op_enqueue(void *op);
void   tp_op_destroy(void *op);

/* VM Object-Dictionary / CAN transport primitives. Each returns 0 on success
   (low byte 0 == ok in the OEM convention). Modelled, not transcribed. */
uint32_t vm_od_send_erase(void *od, uint8_t node, const uint16_t frame[4]);    /* FUN_0013e1c0 */
uint32_t vm_od_send_page_word(void *od, uint8_t node, const uint64_t *word);   /* FUN_0013e310 */
uint32_t vm_od_send_crc_query(void *od, uint8_t node, uint16_t page_be);       /* FUN_0013e3a0 */
void     vm_od_enter_size_mode(void *od, uint8_t node);                        /* FUN_0013e0b0 */
uint32_t vm_od_set_transfer_mode(void *od, uint8_t node, const uint8_t *flag); /* FUN_0013e7a0 */
void     tp_od_watch(void *od, uint32_t id_a, uint32_t id_b, uint8_t node, int len); /* FUN_0016bfa0 */

/* Sub-microsecond settle delay (OEM uses nanosleep with EINTR retry). */
void   tp_sleep_us(int64_t us);

/* Base ctor / vtable wiring - OEM FUN_00139660 + the PTR_FUN_* vtables. */
void   tp_base_ctor(TpClient *c);
void   tp_condvar_init(TpClient *c);
extern const void *PanasonicUpdateClient_vtable;   /* PTR_FUN_0019d1b8 */
extern const void *DynapackUpdateClient_vtable;    /* PTR_FUN_0019d180 */
extern const void *LiteonUpdateClient_vtable;      /* PTR_FUN_0019d1f0 */

/* Liteon VM callback registration + the error path (OEM FUN_0013d620 + the
   runtime_error throw block). */
int    tp_register_vm_callback(TpClient *c, void *vm_handle, const char *name,
                               bool (*cb)(TpClient *, const void *));
void   tp_throw_vm_call_failed(const char *name);   /* throws std::runtime_error */

/* Clocks + condvar/mutex plumbing (std::chrono steady/system + pthreads). */
int64_t tp_steady_now(void);   /* std::chrono::steady_clock::now() in ns */
int64_t tp_system_now(void);   /* std::chrono::system_clock::now() in ns */
void   tp_result_lock(TpClient *c);
void   tp_result_unlock(TpClient *c);
void   tp_result_timedwait(TpClient *c, int64_t abs_ns);  /* condvar @0xc0 */
void   tp_result_notify_all(TpClient *c);
void   tp_charger_lock(TpClient *c);
void   tp_charger_unlock(TpClient *c);
void   tp_charger_timedwait(TpClient *c, int64_t abs_ns); /* condvar @0x140 */

/* Update-file accessors (OEM FUN_00123ae0/FUN_00123b30 + the read step). */
void   tp_read_update_file(TpClient *c, void *update_file);
void   tp_update_file_devname(str_t *out, void *update_file);
void   tp_parse_charger_status(str_t *out, void *update_file, const void *frame);
short  tp_frame_id(const void *frame);             /* *(short*)(frame+6) */

/* Post-transfer hook (OEM FUN_00136f70). */
void   tp_transfer_post_step(TpClient *c);

/* Liteon expected charger-status reference strings (DAT_001a2678/0x001a2698). */
extern const str_t *g_charger_status_2000;
extern const str_t *g_charger_status_5000;

/* Cross-references within this TU. */
uint32_t tp_crc32(const uint8_t *data, size_t len);
void thirdparty_client_set_update_result(TpClient *c, const str_t *reason, char result_code);
void thirdparty_client_advance_page(TpClient *c);
int  thirdparty_client_request_flash_size(TpClient *c);
uint32_t thirdparty_client_start_transfer(TpClient *c, bool on);
uint32_t tp_start_transfer(TpClient *c, int on);  /* enter device transfer mode */
bool liteon_on_charger_status(TpClient *c, const void *frame);
/* ========================================================================== */

/*
 * thirdparty_update_client.c - VanMoof "update" OTA service
 *
 * Reconstruction of the third-party (battery / charger) firmware update
 * client: a page-CRC-over-CAN flash state machine shared by the Panasonic,
 * Dynapack and Liteon battery vendors.  Behaviour-oriented translation of the
 * decompiled AArch64 image (program "update", image base 0x100000).  Source
 * path baked into the binary: devices/main/update/src/thirdparty_update_client.cpp
 *
 * Framework (std::string, nlohmann::json, the VM OD transport, the IUpdateClient
 * vtable) is modelled via update_common.h + the externs declared alongside this
 * file - not rebuilt.  The CAN/OD send helpers, the mutex/condvar plumbing and
 * the update-file buffer are kept as opaque externs; the algorithm (states,
 * per-page CRC w/ retries, full-image CRC, page advance, progress %, the
 * byteswapped bounds check and the per-vendor timing constants) is verbatim.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Update-client state machine (field offsets shown as in the OEM struct).   */
/* ------------------------------------------------------------------------- */

/*
 * Flash FSM states held at offset 0xf4 (TpClient.state):
 *   2 = flash-size-info     (awaiting the device's page-size/page-count reply)
 *   3 = page-CRC            (per-page erase+send, awaiting the page CRC reply)
 *   4 = image-CRC           (awaiting the full-image CRC reply)
 *   5 = update-request      (image verified, awaiting the apply/commit reply)
 *   7 = terminal            (result latched; FSM frozen)
 */
enum tp_state {
    TP_STATE_SIZE_INFO     = 2,
    TP_STATE_PAGE_CRC      = 3,
    TP_STATE_IMAGE_CRC     = 4,
    TP_STATE_UPDATE_REQ    = 5,
    TP_STATE_TERMINAL      = 7,
};

/* CAN command ids passed to send_command() as the "timeout seconds" selector. */
#define TP_CMD_TIMEOUT_ADVANCE      10   /* DAT_00174c50 - AdvancePage / Update request */
#define TP_CMD_TIMEOUT_FULL_CRC      5   /* DAT_00174c40 - CRC over full image          */
#define TP_CMD_TIMEOUT_SIZE_REQ      3   /* DAT_00174c48 - Initial flash size request   */

/* 16-bit byteswap, as emitted inline throughout the FSM. */
static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ------------------------------------------------------------------------- */
/* CRC32 (reflected, poly 0xEDB88320) - OEM FUN_0016ee70.                     */
/* Standard zlib CRC; seed 0xffffffff, final XOR 0xffffffff.                  */
/* ------------------------------------------------------------------------- */
/* OEM 0x16ee70 */
uint32_t tp_crc32(const uint8_t *data, size_t len) {
    uint32_t crc;
    const uint8_t *end;

    if (len == 0)
        return 0;

    end = data + len;
    crc = 0xffffffff;
    do {
        uint32_t b = *data;
        int i = 8;
        do {
            uint32_t mix = b ^ crc;
            uint32_t shifted = crc >> 1;
            b >>= 1;
            crc = (mix & 1) ? (shifted ^ 0xedb88320u) : shifted;
            i--;
        } while (i != 0);
        data++;
    } while (data != end);

    return ~crc;
}

/* ------------------------------------------------------------------------- */
/* CAN transport helpers - thin wrappers over the VM OD send path.           */
/* (modelled; real bodies are FUN_0013e1c0 / FUN_0013e310 / FUN_0013e3a0 /   */
/*  FUN_0013e0b0 / FUN_0013e7a0 in the image.)                               */
/* ------------------------------------------------------------------------- */

/* OEM 0x137480 - erase one flash page on the device, then settle. */
/* Returns 0 on success (matches OEM truth: low byte 0 == ok). */
static uint32_t tp_send_erase_page(TpClient *c, uint32_t page) {
    uint16_t frame[4];
    uint32_t rc;

    /* Frame: word0 = byteswapped page index (low 16 of the swapped 24-bit
       field), word1 = 0x0100 (erase opcode). Exactly as built inline. */
    frame[0] = (uint16_t)(((page >> 8) & 0xff) | ((page & 0x00ff00ffu) << 8));
    frame[1] = 0x0100;

    rc = vm_od_send_erase(c->od_handle, c->od_node, frame);
    if (((rc & 0xff) == 0) && c->step_delay_us != 0 && c->step_delay_us > 0)
        tp_sleep_us(c->step_delay_us);          /* field 0x48 = param[9] */
    return rc;
}

/*
 * OEM 0x137580 - erase + send a single page's data, byte-by-byte over CAN.
 *   - bounds: page must be < page_count
 *   - erase the page first
 *   - build a page_size buffer pre-filled with 0xFF, memcpy the matching slice
 *     of the update image into it (tail past EOF stays 0xFF padded)
 *   - compute CRC32 of the WHOLE page buffer into expected_crc (0x134)
 *   - stream the page as 8-byte words, sleeping inter_word_delay_us between
 *     words (field 0x50 = param[10])
 * Returns true on success, false on any send error / out-of-range page.
 */
/* OEM 0x137580 */
static bool tp_send_page(TpClient *c, uint16_t page) {
    uint8_t *buf;
    size_t page_size;
    size_t off;
    size_t avail;
    size_t i, words;

    if ((uint64_t)page >= c->page_count)
        return false;

    if (tp_send_erase_page(c, page) != 0)
        return false;

    page_size = (size_t)c->page_size;
    buf = NULL;
    if (page_size != 0) {
        buf = (uint8_t *)operator_new(page_size);
        memset(buf, 0xff, page_size);
        page_size = (size_t)c->page_size;   /* re-read, as OEM does */
    }

    off = (size_t)page * page_size;
    avail = (size_t)(c->file_end - c->file_begin);
    if (off < avail) {
        size_t n = avail - off;
        if (page_size < n)
            n = page_size;
        memcpy(buf, c->file_begin + off, n);
    }

    c->expected_crc = tp_crc32(buf, page_size);

    words = page_size >> 3;                  /* 8-byte words */
    for (i = 0; i < words; i++) {
        uint64_t word;
        memcpy(&word, buf + i * 8, 8);
        if (vm_od_send_page_word(c->od_handle, c->od_node, &word) != 0)
            break;                            /* send error: bail (OEM ok=0) */
        if (c->inter_word_delay_us != 0 && c->inter_word_delay_us > 0)
            tp_sleep_us(c->inter_word_delay_us);   /* field 0x50 = param[10] */
    }

    if (buf)
        operator_delete(buf);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Generic queued CAN command - OEM FUN_00137fb0.                            */
/* Enqueues a named command with a completion (done) + error callback and a  */
/* timeout (seconds * 1000 -> ms). On enqueue failure the update is failed   */
/* with the command name as the reason. Stores the pending op at 0x88.       */
/* ------------------------------------------------------------------------- */
/* OEM 0x137fb0 */
static void tp_send_command(TpClient *c, const str_t *name, void *ctx,
                            int timeout_secs) {
    void *op;

    /* Cancel/destroy any in-flight op (field 0x88). */
    if (c->pending_op) {
        tp_op_destroy(c->pending_op);
        operator_delete_sz(c->pending_op, 0x58);
        c->pending_op = NULL;
    }

    op = tp_op_create(c->transport, name, ctx, (int64_t)timeout_secs * 1000);
    if (!tp_op_enqueue(op)) {
        /* Could not even enqueue: fail with the command string as reason. */
        thirdparty_client_set_update_result(c, name, UPD_FAILED);
        tp_op_destroy(op);
        operator_delete_sz(op, 0x58);
    } else {
        c->pending_op = op;
    }
}

/* ------------------------------------------------------------------------- */
/* AdvancePage - OEM FUN_001382c0.                                           */
/* Bumps the current-page counter and queues the "AdvancePage" command,      */
/* whose completion callback (FUN_00137d30) initiates the next page erase.   */
/* ------------------------------------------------------------------------- */
/* OEM 0x1382c0 */
void thirdparty_client_advance_page(TpClient *c) {
    str_t name;
    str_init(&name, "AdvancePage");           /* s_AdvancePage_00173fe8, len 0xb */
    c->current_page = (uint16_t)(c->current_page + 1);
    tp_send_command(c, &name, c, TP_CMD_TIMEOUT_ADVANCE);
    str_free(&name);
}

/* ------------------------------------------------------------------------- */
/* set_update_result - OEM 0x137790.                                         */
/* Latches the terminal result exactly once: records the reason string,      */
/* moves the FSM to state 7, logs success/failure, sets the result code/flag */
/* under the result mutex, and wakes all waiters.                            */
/* ------------------------------------------------------------------------- */
/* OEM 0x137790 */
void thirdparty_client_set_update_result(TpClient *c, const str_t *reason,
                                         char result_code) {
    if (c->state == TP_STATE_TERMINAL)
        return;

    if (!c->reason_set) {
        str_assign(&c->reason, reason);
        c->reason_set = true;
    } else {
        str_assign(&c->reason, reason);
    }
    c->state = TP_STATE_TERMINAL;

    tp_result_lock(c);
    if (!c->result_flag) {
        if (result_code == 0) {
            common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                        0x188, LOG_INFO, "Update succeeded");
        } else {
            common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                        0x18a, LOG_ERR, "Update failed. Reason: %s",
                        str_cstr(reason));
        }
        c->result_code = result_code;
        c->result_flag = true;
    }
    tp_result_unlock(c);

    tp_result_notify_all(c);
}

/* ------------------------------------------------------------------------- */
/* on_flash_size_info - OEM 0x1383e0.                                        */
/* Device replied to the "Initial flash size request" with its page geometry */
/* (two big-endian 16-bit fields packed in the first CAN word). Validates    */
/* state==2, decodes page_size/page_count, checks the image fits, then jumps */
/* to the page-CRC phase and kicks off the first page (AdvancePage).         */
/* ------------------------------------------------------------------------- */
/* OEM 0x1383e0 */
void thirdparty_client_on_flash_size_info(TpClient *c, const uint8_t *payload) {
    uint32_t raw;
    uint32_t page_size;
    uint32_t page_count;
    uint64_t image_len;

    /* Clear the pending op (0x88). */
    if (c->pending_op) {
        tp_op_destroy(c->pending_op);
        operator_delete_sz(c->pending_op, 0x58);
        c->pending_op = NULL;
    }

    if (c->state != TP_STATE_SIZE_INFO) {
        str_t reason;
        str_init(&reason, "Flash size info arrived at an unexpected moment");
        thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
        str_free(&reason);
        return;
    }

    memcpy(&raw, payload, 4);
    c->current_page = 0;                                  /* 0x130 <- 0 */

    /* page_size  = byteswap of the low  16 bits of the reply word */
    page_size = ((raw & 0xffff) >> 8) | ((raw & 0xff) << 8);
    c->page_size = page_size;
    /* page_count = byteswap of the high 16 bits of the reply word */
    page_count = (((raw >> 0x10) << 0x18) | ((raw >> 0x18) << 0x10)) >> 0x10;
    c->page_count = page_count;

    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                0x1a3, LOG_INFO, "page size , page count %d %d",
                page_size, page_count);

    /* The image (plus a 4-byte trailer) must fit in page_size*page_count. */
    image_len = (uint64_t)(c->file_end - c->file_begin) + 4;
    if (image_len <= (uint64_t)c->page_size * (uint64_t)c->page_count) {
        c->state = TP_STATE_PAGE_CRC;
        thirdparty_client_advance_page(c);
        return;
    }

    /* Doesn't fit: build "Reported flash size (<sz>) is not big enough for
       the update (<need>)." and fail. */
    {
        str_t msg;
        char num[32];

        str_init(&msg, "Reported flash size (");
        tp_snprintf(num, sizeof num, "%lu",
                    (unsigned long)((uint64_t)c->page_size * c->page_count));
        str_append(&msg, num);
        str_append(&msg, ") is not big enough for the update (");
        tp_snprintf(num, sizeof num, "%lu", (unsigned long)image_len);
        str_append(&msg, num);
        str_append(&msg, ").");
        thirdparty_client_set_update_result(c, &msg, UPD_FAILED);
        str_free(&msg);
    }
}

/* ------------------------------------------------------------------------- */
/* on_crc_response - OEM 0x139000. The headline state transition.            */
/*                                                                           */
/*  - The reply word carries the device's computed CRC, BIG-ENDIAN; it is    */
/*    byteswapped to host order (local_54) before comparing with our         */
/*    expected_crc (0x134).                                                  */
/*                                                                           */
/*  state == 3 (page CRC):                                                   */
/*     match  -> log progress %, reset retry counter, advance to next page;  */
/*               on the last page move to state 4 and issue the full-image   */
/*               CRC request (over a page_size*page_count 0xFF-padded image). */
/*     mismatch-> bump retry; after the 3rd failure fail the update with     */
/*               "CRC check of a page failed 3 times", else re-send the page */
/*               (AdvancePage / FUN_001382c0).                               */
/*  state == 4 (image CRC):                                                  */
/*     match  -> move to state 5, (lazily compute image CRC if mode==0),     */
/*               queue the final "Update request" (apply) command.           */
/*     mismatch-> fail "CRC check of the entire image failed".               */
/*  state == 7 -> ignore.                                                    */
/*  otherwise -> fail "CRC received at an unexpected time".                  */
/* ------------------------------------------------------------------------- */
/* OEM 0x139000 */
void thirdparty_client_on_crc_response(TpClient *c, const uint8_t *payload) {
    uint32_t raw;
    uint32_t crc;
    int state;

    /* Clear the pending op (0x88). */
    if (c->pending_op) {
        tp_op_destroy(c->pending_op);
        operator_delete_sz(c->pending_op, 0x58);
        c->pending_op = NULL;
    }

    memcpy(&raw, payload, 4);
    state = c->state;
    /* big-endian -> host: reverse all four bytes of the reply word */
    crc = ((raw & 0x000000ffu) << 24) | ((raw & 0x0000ff00u) << 8) |
          ((raw & 0x00ff0000u) >> 8)  | ((raw & 0xff000000u) >> 24);

    if (state == TP_STATE_IMAGE_CRC) {
        if (c->expected_crc == crc) {
            int mode = c->mode;                 /* field 0x68 = param[0xd] */
            c->state = TP_STATE_UPDATE_REQ;

            /* mode==0 vendors compute the image CRC lazily here, over just
               the bytes actually present (no page padding). */
            if (mode == 0) {
                c->expected_crc = tp_crc32(c->file_begin,
                                           (size_t)(c->file_end - c->file_begin));
            }

            {
                str_t name;
                str_init(&name, "Update request");   /* len 0xe */
                tp_send_command(c, &name, c, TP_CMD_TIMEOUT_ADVANCE);
                str_free(&name);
            }
            return;
        }

        {
            str_t reason;
            str_init(&reason, "CRC check of the entire image failed");
            thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
            str_free(&reason);
        }
        return;
    }

    if (state == TP_STATE_TERMINAL)
        return;

    if (state != TP_STATE_PAGE_CRC) {
        str_t reason;
        str_init(&reason, "CRC received at an unexpected time");
        thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
        str_free(&reason);
        return;
    }

    /* ---- state == 3: per-page CRC ---- */
    uint16_t page;
    if (c->expected_crc == crc) {
        /* Page verified. Progress = (current_page * 100) / page_count. */
        uint32_t pct = 0;
        if (c->page_count != 0)
            pct = ((uint32_t)c->current_page * 100u) / (uint32_t)c->page_count;
        common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                    0x1c9, LOG_INFO, "Progress: %d %%", pct);
        c->retry_count = 0;
        page = c->current_page;
    } else {
        /* Page failed. Step the page index back, bump retries. */
        uint32_t retries = c->retry_count;
        page = (uint16_t)(c->current_page - 1);
        c->current_page = page;
        c->retry_count = retries + 1;
        if (retries + 1 > 2) {
            str_t reason;
            str_init(&reason, "CRC check of a page failed 3 times");
            thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
            str_free(&reason);
            return;
        }
    }

    if (c->page_count == page) {
        /* Last page done: switch to full-image CRC. Build the verification
           image: page_size*page_count bytes, 0xFF-filled, with the real
           image copied over the front, then CRC32 the whole thing. */
        uint64_t total = (uint64_t)page * (uint64_t)c->page_size;
        uint8_t *img = NULL;

        c->state = TP_STATE_IMAGE_CRC;

        if (total != 0) {
            img = (uint8_t *)operator_new((size_t)total);
            memset(img, 0xff, (size_t)total);
        }
        memcpy(img, c->file_begin, (size_t)(c->file_end - c->file_begin));
        c->expected_crc = tp_crc32(img, (size_t)total);

        {
            str_t name;
            str_init(&name, "CRC over full image");   /* len 0x13 */
            tp_send_command(c, &name, c, TP_CMD_TIMEOUT_FULL_CRC);
            str_free(&name);
        }
        operator_delete(img);
    } else {
        /* More pages remain: advance to the next one. */
        thirdparty_client_advance_page(c);
    }
}

/* ------------------------------------------------------------------------- */
/* wait_charger_type_validated - OEM 0x138d80 (Liteon).                      */
/* Blocks (under the charger-type mutex, condvar at 0x140) until the device  */
/* confirms its charger status matches the expected max-current profile, or  */
/* until a timeout of (field 0x58 = param[11]) * 200000 ns elapses.  On      */
/* timeout it logs the update file device name and fails (-1). On success it */
/* records the validated charger info, starts the transfer, and kicks the    */
/* initial flash-size request.                                               */
/* ------------------------------------------------------------------------- */
/* OEM 0x138d80 */
int thirdparty_client_wait_charger_type_validated(TpClient *c) {
    int64_t deadline;

    tp_charger_lock(c);

    /* deadline = now + param[11] * 200000 ns (steady clock) */
    deadline = tp_steady_now() + (int64_t)c->charger_wait_unit * 200000;

    if (!c->charger_type_ok) {
        for (;;) {
            int64_t target = (deadline - tp_steady_now()) + tp_system_now();
            tp_charger_timedwait(c, target);
            if (tp_system_now() >= target && tp_steady_now() >= deadline) {
                if (!c->charger_type_ok) {
                    str_t devname;
                    tp_update_file_devname(&devname, c->update_file);
                    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                                0xb9, LOG_ERR,
                                "Unable to validate charger type, update file device name :%s",
                                str_cstr(&devname));
                    str_free(&devname);
                    tp_charger_unlock(c);
                    return -1;
                }
                break;
            }
            if (c->charger_type_ok)
                break;
        }
    }

    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                0xbc, LOG_INFO, "charger type is matched with max current");
    tp_charger_unlock(c);

    /* Read the update file into the image buffer (0x70/0x78/0x80). */
    tp_read_update_file(c, c->update_file);

    /* Start the transfer mode on the device, then request flash geometry. */
    tp_start_transfer(c, 1);
    return (signed char)thirdparty_client_request_flash_size(c);
}

/* ------------------------------------------------------------------------- */
/* request_flash_size - OEM 0x138940.                                        */
/* Queues the "Initial flash size request" command, then blocks (under the   */
/* result mutex, condvar at 0xc0) up to (field 0x60 = param[12]) * 1000 ns   */
/* for any result to latch. On timeout it stamps result code 0x1ff (skipped) */
/* and returns it. Clears the pending op afterward.                          */
/* ------------------------------------------------------------------------- */
/* OEM 0x138940 */
int thirdparty_client_request_flash_size(TpClient *c) {
    int64_t deadline;
    str_t name;

    str_init(&name, "Initial flash size request");        /* len 0x1a */
    tp_send_command(c, &name, c, TP_CMD_TIMEOUT_SIZE_REQ);
    str_free(&name);

    tp_result_lock(c);
    deadline = tp_steady_now() + (int64_t)c->result_wait_unit * 1000;
    if (!c->result_flag) {
        for (;;) {
            int64_t target = (deadline - tp_steady_now()) + tp_system_now();
            tp_result_timedwait(c, target);
            if (tp_system_now() >= target && tp_steady_now() >= deadline) {
                if (!c->result_flag) {
                    /* timeout: result_code/flag <- 0x1ff (low byte ff = skip) */
                    c->result_code = (char)0xff;
                    c->result_flag = true;
                }
                break;
            }
            if (c->result_flag)
                break;
        }
    }
    tp_result_unlock(c);

    if (c->pending_op) {
        void *op = c->pending_op;
        c->pending_op = NULL;
        tp_op_destroy(op);
        operator_delete_sz(op, 0x58);
    }

    return (unsigned char)c->result_code;
}

/* ------------------------------------------------------------------------- */
/* Constructors - per-vendor timing/mode tuning.  The base ctor (FUN_00139660)
 * wires the vtable + OD registration; only the vendor-specific tunables are
 * reconstructed here (the values are verbatim from the disassembly).
 *
 * Field layout (8-byte words / byte offsets):
 *   [9]  0x48  step_delay_us       (erase / transfer-step settle, us)
 *   [10] 0x50  inter_word_delay_us (between 8-byte page words, us)
 *   [11] 0x58  charger_wait_unit / settle
 *   [12] 0x60  result_wait_unit    (flash-size request timeout unit)
 *   [0xd]0x68  mode                (1 => lazy image CRC skipped; 0 => lazy)
 * ------------------------------------------------------------------------- */

/* OEM 0x13b6a0 - Panasonic. `fast` selects the short timing profile. */
void PanasonicUpdateClient_ctor(TpClient *c, bool fast) {
    tp_base_ctor(c);
    c->vptr = &PanasonicUpdateClient_vtable;     /* PTR_FUN_0019d1b8 */
    if (!fast) {
        c->step_delay_us       = 300000;
        c->inter_word_delay_us = 30000;
        c->charger_wait_unit   = 30000;
        c->result_wait_unit    = 900000000;
    } else {
        c->step_delay_us       = 100000;
        c->inter_word_delay_us = 10000;
        c->charger_wait_unit   = 10000;
        c->result_wait_unit    = 360000000;
    }
    c->mode = 1;
    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                0x81, LOG_INFO, "Panasonic Update Client");
}

/* OEM 0x13b620 - Dynapack. */
void DynapackUpdateClient_ctor(TpClient *c) {
    tp_base_ctor(c);
    c->vptr = &DynapackUpdateClient_vtable;      /* PTR_FUN_0019d180 */
    c->step_delay_us       = 50000;
    c->inter_word_delay_us = 1000;
    c->charger_wait_unit   = 0;
    c->mode = 0;
    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                0x53, LOG_INFO, "Dynapack Update Client");
}

/* OEM 0x13b750 - Liteon. Also subscribes to the device's "charger status"
 * notification (FUN_0013d620 / FUN_00137140 callback at field 0x13c) and
 * arms the OD watch (FUN_0016bfa0, id 0xffffffa3/0xffffff8a, len 0x12). */
void LiteonUpdateClient_ctor(TpClient *c, void *vm_handle) {
    tp_base_ctor(c);
    c->vptr = &LiteonUpdateClient_vtable;        /* PTR_FUN_0019d1f0 */
    c->charger_type_ok = false;
    tp_condvar_init(c);
    c->mode = 0;
    c->step_delay_us       = 20000;
    c->inter_word_delay_us = 10000;
    c->charger_wait_unit   = 10000;
    common_logf("devices/main/update/src/thirdparty_update_client.cpp",
                0xa7, LOG_INFO, "Liteon Update Client");

    /* Register the "charger_status" notification handler (FUN_00137140). */
    if (tp_register_vm_callback(c, vm_handle, "charger_status",
                                liteon_on_charger_status) != 0) {
        tp_throw_vm_call_failed("charger_status");
    }
    /* Arm the OD watch for charger-status frames. */
    tp_od_watch(c->od_handle, 0xffffffa3u, 0xffffff8au, c->od_node, 0x12);
}

/* ------------------------------------------------------------------------- */
/* on_advance_page_done - OEM 0x137d30 (AdvancePage completion callback).    */
/* Decrement-then-erase: steps the page counter back by one, then triggers   */
/* the actual page send/erase (tp_send_page). On failure fails the update    */
/* with "Failed to initiate page erase, page send, or page crc query";       */
/* on success re-increments so the page index is preserved for the CRC reply.*/
/* ------------------------------------------------------------------------- */
/* OEM 0x137d30 */
bool thirdparty_client_on_advance_page_done(TpClient *c) {
    uint16_t page;
    bool ok;

    page = (uint16_t)(c->current_page - 1);
    c->current_page = page;

    /* send this page; the device then reports the page CRC.  The inner
       FUN_0013e3a0 re-issues the CRC query (page index byteswapped). */
    ok = tp_send_page(c, page) &&
         (vm_od_send_crc_query(c->od_handle, c->od_node, bswap16(page)) == 0);

    if (!ok) {
        str_t reason;
        str_init(&reason,
                 "Failed to initiate page erase, page send, or page crc query");
        thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
        str_free(&reason);
        return false;
    }

    c->current_page = (uint16_t)(c->current_page + 1);
    return true;
}

/* ------------------------------------------------------------------------- */
/* on_update_begin - OEM 0x1378d0 (initial flash-size-request completion).   */
/* Guards against a double-start (state not in {0,2}) by failing "Multiple   */
/* updates requested", then sets state=2 and tells the device to enter the   */
/* flash-size reporting mode (FUN_0013e0b0).                                 */
/* ------------------------------------------------------------------------- */
/* OEM 0x1378d0 */
bool thirdparty_client_on_update_begin(TpClient *c) {
    if ((c->state & 0xfffffffdu) != 0) {
        str_t reason;
        str_init(&reason, "Multiple updates requested");
        thirdparty_client_set_update_result(c, &reason, UPD_FAILED);
        str_free(&reason);
    }
    c->state = TP_STATE_SIZE_INFO;
    vm_od_enter_size_mode(c->od_handle, c->od_node);
    return true;
}

/* ------------------------------------------------------------------------- */
/* start_transfer - OEM 0x137060.                                            */
/* Tells the device to enter/leave transfer mode (FUN_0013e7a0); on a 0 (ok) */
/* return it runs the post-step hook (FUN_00136f70).                         */
/* ------------------------------------------------------------------------- */
/* OEM 0x137060 */
uint32_t thirdparty_client_start_transfer(TpClient *c, bool on) {
    uint8_t flag = (uint8_t)on;
    uint32_t rc = vm_od_set_transfer_mode(c->od_handle, c->od_node, &flag);
    if ((rc & 0xff) == 0)
        tp_transfer_post_step(c);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* liteon_on_charger_status - OEM 0x137140 (Liteon notification handler).    */
/* Parses the device's charger_status reply; sets charger_type_ok (0x13c)    */
/* when the reported status string equals the expected one for the device's  */
/* max-current class. id 2000 vs 5000 selects which reference string to      */
/* compare against (DAT_001a2678 / DAT_001a2698 with their lengths).         */
/* ------------------------------------------------------------------------- */
/* OEM 0x137140 */
bool liteon_on_charger_status(TpClient *c, const void *frame) {
    str_t status;
    short id;
    bool ok = false;

    id = tp_frame_id(frame);                       /* *(short*)(frame+6) */
    tp_parse_charger_status(&status, c->update_file, frame);

    if (id == 2000) {
        ok = str_equals(&status, g_charger_status_2000);
    } else if (id == 5000) {
        ok = str_equals(&status, g_charger_status_5000);
    }
    c->charger_type_ok = ok;

    str_free(&status);
    return false;
}