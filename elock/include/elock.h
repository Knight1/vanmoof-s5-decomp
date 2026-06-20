#ifndef ELOCK_H
#define ELOCK_H

/*
 * elock.h — shared declarations for the VanMoof S5/A5 electronic frame-lock
 * sub-ECU application layer.
 *
 * Reconstructed from the OEM image
 *   elock.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC546xx Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). Only VanMoof application code is reconstructed here; the
 * FreeRTOS kernel + Cortex-M port, the NXP SDK/HAL (motor drive, M_CAN, ADC,
 * flash), any AES block-cipher primitive and libgcc are vendor (declared
 * `extern` and satisfied upstream at link time).
 *
 * RAM globals are written as literal-address volatile accesses so the emitted
 * code matches the OEM (which loads the absolute address from a literal pool).
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ------------------------------------------------------------------ types */
/* CAN Object-Dictionary signal record (only the touched fields are named). */
typedef struct elock_od_record {
    uint8_t  *data;        /* +0x00 multi-frame data pointer */
    uint8_t   _pad04[4];   /* +0x04 */
    uint32_t  length;      /* +0x08 total byte count (multi-frame) */
    uint32_t  can_id;      /* +0x0c 24-bit CAN id */
    uint8_t   mode;        /* +0x10 0=local 1/2=remote multi-frame */
    uint8_t   _pad11[0x0f];/* +0x11 */
    uint8_t  *payload;     /* +0x20 single-frame payload pointer */
    uint32_t  payload_len; /* +0x24 single-frame payload length */
} elock_od_record_t;

/* ----------------------------------------------- vendor callees (deferred) */
/* FreeRTOS kernel + Cortex-M port + message buffers + software timers */
extern void    *msgbuf_create(void);                                                  /* vendor freertos // 0x00006836 */
extern void    *rtos_msgbuf_alloc(uint32_t n, uint32_t sz);                            /* vendor freertos // 0x000067c6 */
extern void     rtos_msgbuf_delete(int handle);                                       /* vendor freertos // 0x0000131c */
extern int      rtos_msgbuf_receive(void *handle, uint32_t ticks);                     /* vendor freertos // 0x0000160c */
extern int      rtos_msgbuf_send(void *handle, int x);                                 /* vendor freertos // 0x00002030 */
extern int      message_buffer_send(void *buf, void *tag, uint32_t r2, const void *data, uint32_t len);  /* vendor freertos // 0x0000626c */
extern void     rtos_task_delay(uint32_t ticks);                                       /* vendor freertos // 0x000015c0 */
extern void     rtos_lock_a(void);                                                     /* vendor freertos // 0x00000e44 (vTaskSuspendAll/enter) */
extern void     rtos_unlock_a(void);                                                   /* vendor freertos // 0x00000e60 */
extern void     rtos_assert(void);                                                     /* vendor freertos // 0x00005a98 (configASSERT) */
extern void     rtos_yield_isr(void);                                                  /* vendor freertos // 0x00005ada */
extern void     mb_reclaim(int p);                                                     /* vendor freertos // 0x00005b4c */
extern void     mb_assign(int a, int b);                                               /* vendor freertos // 0x00005a40 */
extern void     timer_stop(void *timer);                                               /* vendor freertos // 0x00002768 */
extern void     timer_complete_cb(void *cb, void *a, void *b);                         /* vendor freertos // 0x00002898 */
extern void     elock_timeout_arm(void *handle, int ticks, int oneshot, void *cbarg, int src_tag);  /* vendor freertos // 0x0000325c (sw-timer create+start) */
/* libgcc / compiler runtime */
extern void     mem_set(void *dst, int val, uint32_t len);                             /* vendor libgcc // 0x00006bda (memset) */
extern void     mem_copy(void *dst, const void *src, uint32_t len);                    /* vendor libgcc // 0x00006bc0 (memcpy) */
extern int      mem_compare(const void *a, const void *b, uint32_t len);               /* vendor libgcc // 0x00006ba0 (memcmp) */
/* NXP SDK/HAL — M_CAN, NVIC, NVM page driver */
extern int      m_can_index(volatile void *base);                                      /* vendor hal // 0x00000b54 */
extern void     m_can_set_baud(uint16_t v);                                            /* vendor hal // 0x00005886 */
extern void     m_can_set_clock(uint32_t v);                                           /* vendor hal // 0x00005a00 */
extern int      m_can_apply(int base);                                                 /* vendor hal // 0x0000064c */
extern void     nvic_enable(int irq);                                                  /* vendor hal // 0x00000378 */
extern int      nvm_record_open(void);                                                 /* vendor hal // 0x00002f50 */
extern int      nvm_record_read(int proxy, void *dst, int rec_id, int len);            /* vendor hal // 0x00000b2c */
extern int      nvm_record_write_verify(int proxy, const void *rec, int len, int off); /* vendor hal // 0x00006a44 */
extern int      nvm_record_finalize(int proxy, int flag);                              /* vendor hal // 0x00002f98 */
extern void     nvm_record_close(int proxy);                                           /* vendor hal // 0x00005b6c */
/* CAN Object-Dictionary comms-registry middleware (generic, shared across sub-ECUs) */
extern void    *od_registry_lookup(uint32_t registry, uint32_t descriptor);            /* vendor hal // 0x00005910 */
extern int      od_registry_wait(void *lock, int ticks);                               /* vendor hal // 0x00005da6 */
extern void     od_registry_release(void *lock);                                       /* vendor hal // 0x000061d4 */
extern int      od_frame_get(void *comms, int n);                                      /* vendor hal // 0x000060c2 */
extern int      od_frame_flush(void *comms);                                           /* vendor hal // 0x00006192 */
extern void     registry_teardown_enter(int a, int b, int c);                          /* vendor hal // 0x00002598 */
extern void     registry_purge(void);                                                  /* vendor hal // 0x00000d54 */
extern void     registry_finalize(void);                                               /* vendor hal // 0x000038f4 */

/* --- VanMoof callees not carved this batch (forward decls) --- */
int  elock_can_bus_reset(void *comms);  /* 0x000035c0 (M_CAN/OD bus reset+recover) */
/* elock_main_init (0x0000396a) — deferred (predominantly NXP-SDK peripheral bring-up). */

/* --------------------------------------------------------- VanMoof prototypes */
void lock_motor_pwm_set_duty(unsigned int pct);                                        /* 0x000003cc */
void elock_unlock_retry_arm(void *ctx);                                                /* 0x00003450 */
void elock_log_event(uint32_t src_tag, uint16_t code, int argc, ...);                  /* 0x0000352c */
void elock_lock_task(uint32_t *t, uint32_t arg1);                                      /* 0x00004eb0 */
void can_send_multiframe(void *bus, elock_od_record_t *rec, uint8_t *frame, uint32_t len);  /* 0x00005ce6 */
void od_signal_write_u16(void *ctx, const uint16_t *value);                            /* 0x0000620c */
int  elock_can_send_signal(void *bus, elock_od_record_t *rec);                         /* 0x00006644 */
int  elock_od_signal_8808_send(void *bus, const uint32_t *value);                      /* 0x000066ca */
int  elock_load_secret_to_od_87(uint32_t *ctx, int out);                               /* 0x0000685a */
int  elock_load_aeskey_to_od_91(uint32_t *ctx, int out);                               /* 0x0000695a */
int  elock_store_secret_87(uint32_t *ctx, int unused, const void *secret);             /* 0x00006ac2 */
int  elock_store_aeskey_91(uint32_t *ctx, int unused, const void *aeskey);             /* 0x00006b22 */

#endif /* ELOCK_H */
