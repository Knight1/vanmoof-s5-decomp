/*
 * event.c — VanMoof input / button-scan event handling.
 *
 * Functions:
 *   event_notify_post       @ 0x00003cf4  (post a 13-byte notify record)
 *   input_event_post        @ 0x00003d14  (post a 3-byte input event record)
 *   event_notify_post_state @ 0x00003d48  (post a fixed 4-byte state-notify record)
 *   xfer_state_lock_post    @ 0x00003d80  (mark a lock held + teardown + notify)
 *   event_post_boot         @ 0x00003dc8  (clock-gate bring-up + post the boot state)
 *   button_scan_poll        @ 0x00003ddc  (per-button GPIO debounce poller)
 *
 * button_scan_poll (0x00003ddc) is the per-button debounce poller. It is
 * installed as a dispatch-table callback (no direct call sites in the image),
 * invoked with a button identifier; it walks the 4-entry GPIO scan table,
 * samples the matching GPIO input, runs a press/release edge state machine and
 * posts the resulting input events on the manager's event stream.
 *
 * GPIO inputs live in the port peripheral window at 0x4008c000: a port is
 * selected with a 0x20-byte stride (port_index << 5) and the pin's input level
 * is the byte at (base + port_index*0x20 + pin_offset).
 *
 * Debounce/repeat timing is handled by the FreeRTOS-timer helper at 0x000015e0
 * (vendor wrapper, extern): 0x3e8 (1000) ticks for the initial press window and
 * 0xc8 (200) ticks for the release/repeat window.
 */

#include <stdint.h>

#include "store.h"      /* timer_remaining_ticks (0x19f8) */
#include "pcc.h"        /* nvic_clockgate_bringup (0x67f4) */

/*
 * Scan-table entry — 0xc (12) bytes per button. Resides in runtime SRAM
 * (.bss @ 0x20000668), populated at init, so the fields are reconstructed from
 * the access pattern in button_scan_poll:
 *
 *   +0x0  u8   event id byte (posted via input_event_post)
 *   +0x1  u8   GPIO port index   (<< 5 to index the 0x4008c000 window)
 *   +0x2  u8   GPIO pin offset    (byte offset within the selected port)
 *   +0x3  u8   match key vs. the incoming button id
 *   +0x4  void* timer handle slot (passed to the FreeRTOS-timer helper)
 *   +0x8  u8   pressed flag (0 = released, 1 = pressed)
 *   +0x9  u8   repeat / hold counter
 */
typedef struct scan_entry {
    uint8_t  event_id;     /* +0x0 */
    uint8_t  port_index;   /* +0x1 */
    uint8_t  pin_offset;   /* +0x2 */
    uint8_t  match_key;    /* +0x3 */
    void    *timer;        /* +0x4 */
    uint8_t  pressed;      /* +0x8 */
    uint8_t  repeat;       /* +0x9 */
    uint8_t  _pad[2];      /* +0xa..+0xb (stride is 0xc) */
} scan_entry_t;

/* GPIO input port window base. // 0x00003e06/0x00003e0a (0x40000000 + 0x8c000) */
#define GPIO_INPUT_BASE   0x4008c000u

/* xfer-state lock object base, arg to xfer_state_lock_post. // DAT_00003ea0 */
#define XFER_STATE_LOCK_OBJ  ((void *)0x20000648u)

/* Scan table base — 4 entries of 0xc bytes in .bss. // DAT_00003ea4 */
#define SCAN_TABLE  ((scan_entry_t *)0x20000668u)

/* Debounce-timer expiry callback (Thumb fn ptr). // DAT_00003ea8 = 0x00009f87 */
extern void scan_timer_expiry(void);

/* Device-manager object slot (published during bring-up). // DAT_00003d40 */
#define DEVMGR_SLOT  (*(void *volatile *)0x200007f0u)

/* input_event_post message tag, sent in the position arg. // DAT_00003d44 */
#define INPUT_EVENT_TAG  0x00009a0bu

/*
 * FreeRTOS xStreamBufferSend (stream_buffer.c, vendor, deferred). Rendered
 * 5-arg by the decompiler — kept ABI-compatible: (buffer, position, ticks,
 * item, length). // 0x0000926c
 */
extern int xStreamBufferSend(void *buffer, uint32_t pos, uint32_t ticks,
                             const void *item, uint32_t len);

/* --- VanMoof event-stream posters (event.c siblings) ---------------------- */

/* xfer_state_lock_post — defined below; forward-declared (kept un-inlined). */
int xfer_state_lock_post(void *lock_obj, uint32_t ticks);

/*
 * Originating object whose +0xc slot holds the device-manager that carries the
 * event message buffer at +0x590. event_notify_post takes this object directly
 * (rather than the fixed DEVMGR_SLOT that input_event_post reads). Only the one
 * field is touched, so the type is kept opaque.
 */
typedef struct event_src {
    uint8_t  _pad00[0x0c];  /* +0x00..+0x0b                            */
    void    *manager;       /* +0x0c device manager (buffer @ +0x590)  */
} event_src_t;

/* Notify-record message tag, sent in the position arg. // DAT_00003d10 */
#define EVENT_NOTIFY_TAG  0x0000a003u

/* Notify payload size: 13 bytes (movs r2,#0xd). */
#define EVENT_NOTIFY_LEN  0x0du

/*
 * event_notify_post — post a 13-byte notify record to the manager queue. // 0x00003cf4
 *
 * Sibling of input_event_post, but the device-manager object is taken from the
 * caller's `src` (its +0xc slot) instead of the fixed SRAM slot. Reads the event
 * stream buffer handle at (manager + 0x590) and pushes a record via
 * xStreamBufferSend: the fixed tag 0xa003 in the position arg, `src` itself in
 * the ticks/context slot (a back-reference to the originating object), and the
 * 13 payload bytes at `payload`.
 *
 * OEM ABI: (src ptr in r0, payload ptr in r1) -> int. The decompiler renders it
 * void; the machine code does not touch r0 after the call (push {r0,r1,r4,lr} ...
 * pop {r4,pc}), so the send's status word is tail-returned.
 */
int event_notify_post(void *src, const void *payload)
{
    /* ldr r4,[r0,#0xc] ; ldr.w r0,[r4,#0x590] */
    void *queue = *(void **)((uint8_t *)((event_src_t *)src)->manager + 0x590);

    /* ldr r1,=0xa003 ; r2=src ; r3=payload ; [sp]=0xd (length) */
    return xStreamBufferSend(queue, EVENT_NOTIFY_TAG, (uint32_t)(uintptr_t)src,
                             payload, EVENT_NOTIFY_LEN);
}

/*
 * input_event_post — post a 3-byte input event record. // 0x00003d14
 *
 * The device-manager object is read from the fixed SRAM slot 0x200007f0; while
 * the slot is null this is a no-op (the cbz gate). The three argument bytes are
 * laid out contiguously into a 3-byte stack record and posted to the manager's
 * event stream buffer at (manager + 0x590) via xStreamBufferSend, carrying the
 * fixed tag 0x00009a0b in the position arg and the manager object itself in the
 * ticks/context slot.
 *
 * OEM ABI: three uint8_t arguments (r0/r1/r2), returns void. The decompiler
 * invents a fourth byte (param_4 >> 0x18) at sp+0xf — phantom: the machine code
 * writes exactly three bytes (sp+0xc..0xe) and sends length 3.
 */
void input_event_post(uint8_t event_id, uint8_t edge, uint8_t arg)
{
    void *mgr = DEVMGR_SLOT;                         /* ldr r4,[r3]; cbz r4 */
    if (mgr != (void *)0) {
        uint8_t  msg[3];
        void    *queue = *(void **)((uint8_t *)mgr + 0x590);  /* ldr r0,[r4,#0x590] */

        msg[0] = event_id;                           /* strb r0,[sp,#0xc] */
        msg[1] = edge;                               /* strb r1,[sp,#0xd] */
        msg[2] = arg;                                /* strb r2,[sp,#0xe] */

        /* r2 = mgr (ticks/context slot); [sp] = 3 (length). */
        xStreamBufferSend(queue, INPUT_EVENT_TAG, (uint32_t)mgr, msg, 3);
    }
}

/*
 * event_notify_post_state — post a fixed 4-byte state-notify record. // 0x00003d48
 *
 * Sibling of event_notify_post (0x3cf4) / input_event_post (0x3d14): takes the
 * manager pointer directly (no NULL gate) and posts the constant payload
 * { 0xc0, 0x05, 0x00, 0x01 } to the event stream buffer at (mgr + 0x590). The
 * epilogue tail-returns the xStreamBufferSend status (r0 untouched after the
 * call), so this is int like its no-gate sibling event_notify_post.
 */
int event_notify_post_state(void *mgr)
{
    uint8_t msg[4];

    msg[0] = 0xc0;                                  /* strb @ sp+0xc */
    msg[1] = 0x05;                                  /* strb @ sp+0xd */
    msg[2] = 0x00;                                  /* strb @ sp+0xe */
    msg[3] = 0x01;                                  /* strb @ sp+0xf */

    /* mgr+0x590 = event stream buffer handle. // 0x3d6c */
    return xStreamBufferSend(*(void **)((uint8_t *)mgr + 0x590),
                             0x00009d19u,            /* DAT_00003d7c */
                             (uint32_t)(uintptr_t)mgr,
                             msg, 4);
}

/*
 * xfer-state lock object. Only the OEM-touched fields are modeled; the
 * surrounding region is opaque so the byte offsets match the image.
 *   +0x00  base       device-manager base struct (event buffer @ +0x590)
 *   +0x04  state      lifecycle state; 2 = held/closing (busy guard)
 *   +0x1c  teardown   optional teardown callback (byte status), 0 = none
 */
typedef struct xfer_lock {
    void   *base;                   /* +0x00 */
    int     state;                  /* +0x04 */
    uint8_t _pad08[0x14];           /* +0x08..+0x1b */
    uint8_t (*teardown)(void);      /* +0x1c */
} xfer_lock_t;

/* Header word 0 stamped into the lock control message. // DAT_00003dc4 */
#define XFER_LOCK_TAG  0x4795u

/*
 * xfer_state_lock_post — mark an xfer-state lock held and notify. // 0x00003d80
 *
 * Rejects a NULL object (-2) or one already in state 2 (-3, busy). Otherwise
 * latches state 2, runs the object's optional teardown callback (capturing its
 * byte status), and posts a header-only control record to the manager's event
 * stream at base+0x590: tag 0x4795 in the position arg, the lock object itself
 * in the ticks/context slot, and a zero-length payload. Returns the
 * sign-extended OR of the send status and the teardown status.
 *
 * OEM ABI: (lock_obj in r0, ticks in r1) -> int. The decompiler renders the call
 * site 1-arg; the machine code forwards r1 into the send's (unused) sixth slot
 * via the push {r0,r1,...} frame, so `ticks` is carried but never consumed by
 * this build's xStreamBufferSend.
 */
int xfer_state_lock_post(void *lock_obj, uint32_t ticks)
{
    xfer_lock_t *lock = (xfer_lock_t *)lock_obj;
    uint8_t      cb_status;
    uint8_t      send_status;

    (void)ticks;                                    /* sp[+4]: unused 6th arg */

    if (lock == (xfer_lock_t *)0) {                 /* cbz r0 -> -2 */
        return -2;
    }
    if (lock->state == 2) {                         /* ldr +0x4 ; cmp #2 -> -3 */
        return -3;
    }

    lock->state = 2;                                /* movs #2 ; str +0x4 */

    if (lock->teardown == (uint8_t (*)(void))0) {   /* ldr +0x1c ; cbz */
        cb_status = 0;                              /* movs r6,#0 ; mov r4,r6 */
    } else {
        cb_status = lock->teardown();               /* blx r3 ; mov r4,r0 */
    }

    /* header-only control record; lock_obj rides the ticks/context slot. */
    send_status = (uint8_t)xStreamBufferSend(
        *(void **)((uint8_t *)lock->base + 0x590),  /* ldr [*lock + 0x590] */
        XFER_LOCK_TAG,                              /* r1 = 0x4795 */
        (uint32_t)(uintptr_t)lock,                  /* r2 = lock_obj */
        (const void *)0,                            /* r3 = 0 (no payload) */
        0);                                         /* sp[0] = 0 (length) */

    return (int)(int8_t)(send_status | cb_status);  /* orrs r0,r4 ; sxtb r0 */
}

/*
 * event_post_boot — clock-gate bring-up, then post the boot xfer-state. // 0x00003dc8
 *
 * Thin wrapper: runs nvic_clockgate_bringup() (0x67f4), then tail-calls the
 * lock/notify poster xfer_state_lock_post (0x3d80) on the fixed xfer-state
 * object (0x20000648), forwarding its int status. The OEM tail-call (b.w 0x3d80)
 * leaves r1 (ticks) unset; the poster ignores it.
 */
int event_post_boot(void)
{
    nvic_clockgate_bringup();                       /* bl 0x000067f4 */
    return xfer_state_lock_post(XFER_STATE_LOCK_OBJ, 0);  /* ldr r0,=0x20000648 ; b.w 0x3d80 */
}

/* --- vendor FreeRTOS-timer helper (0x000015e0, deferred) ------------------ */
/*
 * (handle, ticks, autoreload, ctx, expiry_cb): (re)arms a software timer; the
 * handle is the holder pointer passed by value (the helper dereferences it
 * once internally), i.e. entry->timer — NOT &entry->timer.
 */
extern int rtos_timer_arm(void *handle, uint32_t ticks, int autoreload,
                          void *ctx, void (*expiry_cb)(void));

/* timer_remaining_ticks (0x000019f8) is declared in store.h; the entry's timer
 * holder (entry->timer, a void*) is passed by value. */

/*
 * button_scan_poll — per-button GPIO debounce poller. // 0x00003ddc
 *
 * OEM ABI: (uint button_id) -> void. The decompiler invents a second parameter
 * (param_2) and a trailing arg to both the timer helper and input_event_post;
 * the machine code uses only r0 (button_id) and never sets up those extra
 * arguments, so they are dropped here.
 */
void button_scan_poll(uint32_t button_id)
{
    scan_entry_t *entry;
    int idx;
    char level;

    /* Take the xfer-state lock / notify before scanning. // 0x00003de2 */
    xfer_state_lock_post(XFER_STATE_LOCK_OBJ, 0);   /* ticks slot is dead */

    /* Find the active table entry whose match key == button_id. // 0x00003de8 */
    idx = 0;
    entry = SCAN_TABLE;
    while (entry->timer == 0 || entry->match_key != (uint8_t)button_id) {
        idx++;
        entry++;
        if (idx == 4) {
            return;
        }
    }

    entry = &SCAN_TABLE[idx];

    /* Sample the GPIO input level. // 0x00003e00..0x00003e0e */
    level = *(volatile char *)(GPIO_INPUT_BASE
                               + ((uint32_t)entry->port_index << 5)
                               + (uint32_t)entry->pin_offset);

    /* --- press edge --- // 0x00003e10 */
    if (entry->pressed == 0) {
        if (level != 1) {
            return;
        }
        entry->pressed = 1;

        /* Bump the repeat counter if the timer was already running. // 0x00003e1c
         * OEM passes entry->timer (the holder value), not &entry->timer. */
        if (timer_remaining_ticks((void **)entry->timer) != 0) {
            entry->repeat++;
        }

        /* Arm the 1000 ms press window. // 0x00003e2c..0x00003e42 */
        rtos_timer_arm(SCAN_TABLE[idx].timer, 1000, 1, entry,
                       scan_timer_expiry);
        /* Post the press event. // 0x00003e46..0x00003e4e */
        input_event_post(SCAN_TABLE[idx].event_id, 0, 0);
    }

    /* --- release edge --- // 0x00003e52 */
    if (SCAN_TABLE[idx].pressed != 1) {
        return;
    }
    if (level != 0) {
        return;
    }
    SCAN_TABLE[idx].pressed = 0;

    if (SCAN_TABLE[idx].repeat == 0xff) {
        /* Saturated repeat count: clear and emit a single release. // 0x00003e68 */
        SCAN_TABLE[idx].repeat = 0;
    } else {
        /* Arm the 200 ms release/repeat window. // 0x00003e7e..0x00003e88 */
        rtos_timer_arm(SCAN_TABLE[idx].timer, 200, 1, entry,
                       scan_timer_expiry);
    }

    /* Post the release event. // 0x00003e6a..0x00003e7a (tail call) */
    input_event_post(SCAN_TABLE[idx].event_id, 1, 0);
}
