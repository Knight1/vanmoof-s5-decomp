#ifndef FRONTLIGHT_H
#define FRONTLIGHT_H

/*
 * frontlight.h — shared declarations for the VanMoof S5/A5 front LED light
 * controller application layer.
 *
 * Reconstructed from the OEM image
 *   frontlight.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC546xx Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). Only VanMoof application code is reconstructed here; the
 * FreeRTOS kernel + Cortex-M port, the NXP SDK/HAL (SCTimer/PWM, M_CAN, ADC,
 * flash) and libgcc are vendor (declared `extern`, satisfied upstream at link).
 *
 * RAM/MMIO globals are named below and accessed as literal-address volatile
 * casts so the emitted code matches the OEM (which loads the absolute address
 * from a literal pool). Substituting the name for the hex changes nothing in
 * the emitted code.
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ---------------------------------------------------- device addresses ---- */
/* MMIO */
#define FL_SCT_BASE           0x40085000u   /* SCTimer0 base: EV ctrl +0x304, EV state +0x30c, MATCHREL +0x100/+0x200, output cfg +0xe00/+0xe04/+0xe10 */
/* RAM globals */
#define FL_SCT_EVENT_IDX      0x20000070u   /* ptr-to-ptr: active SCTimer event index */
#define FL_PWM_CTX_PP         0x2000006cu   /* ptr-to-ptr: SCTimer/PWM frame-timer context */
#define FL_LED_CB             0x20000084u   /* LED driver control block (int[] at cb[0..0xf]) */
#define FL_MASTER_BRIGHTNESS  0x2000000cu   /* u8 master brightness (0xff = full, else scale) */
#define FL_FRAME_TABLE        0x200008ccu   /* primary 300-entry frame table (12 B/entry) */
#define FL_LOG_CTX            0x200001e8u   /* ptr-to-ptr: event-publisher context */
/* flash tables / constants */
#define FL_GAMMA_LUT          0x00007cb1u   /* 256-entry u8 gamma LUT (flash) */
#define FL_FRAME_TABLE_WRAP   0x00006b78u   /* secondary/wrap frame table (flash) */
#define FL_LED_CALLBACK       0x000015e0u   /* LED/SCTimer driver completion callback (stored in cb[7]) */
#define FL_LOG_TAG            0x00003a75u   /* message-buffer handle/tag for log records */
#define FL_LOG_SRC_OVERFLOW   0x61a4e2b2u   /* log src_tag for frame-table overflow (code 0x9e) */

/* ----------------------------------------------- vendor callees (deferred) */
extern void  mem_set(void *dst, int c, uint32_t n);                                   /* vendor libgcc // 0x00005ebe (memset) */
extern void  mem_copy(void *dst, const void *src, uint32_t n);                        /* vendor libgcc // 0x00005ea4 (memcpy) */
extern int   mem_compare(const void *a, const void *b, uint32_t n);                   /* vendor libgcc // 0x00005e84 (memcmp) */
extern void  message_buffer_send(void *mb, void *handle, uint32_t flags, void *data, uint32_t len); /* vendor freertos // 0x00005926 */
extern int   rtos_sem_take(void *handle, uint32_t ticks);                             /* vendor freertos // 0x000013cc (queue/sem take; 1=signalled) */
extern void  rtos_sem_give(void *handle, int x);                                      /* vendor freertos // 0x00002270 */
extern void  od_frame_marker(uint32_t marker);                                        /* vendor hal // 0x00002888 (OD begin/end-frame signal) */
extern int   nvm_record_open(void);                                                   /* vendor hal // 0x00001c40 */
extern int   nvm_record_read(int handle, void *buf, uint32_t rec_id, uint32_t len);   /* vendor hal // 0x00000948 */
extern void  nvm_record_release(int handle);                                          /* vendor hal // 0x00004fa6 */
extern void *od_registry_lookup(uint32_t bus, uint32_t descriptor);                   /* vendor hal // 0x00004db2 */
extern int   od_resource_wait(void *res, int timeout);                                /* vendor hal // 0x000051e0 */
extern void  od_signal_send(uint32_t sig);                                            /* vendor hal // 0x0000587a */

/* ------------------------------------------------------------------ types */
/* render argument (anim/window descriptor); unaligned u16 fields match the OEM. */
typedef struct __attribute__((packed)) {
    uint8_t  _pad0;    /* +0 */
    uint8_t  _pad1;    /* +1 */
    uint8_t  flags;    /* +2 bit2 = suppress PWM update */
    uint16_t start;    /* +3 start frame index (unaligned) */
    uint16_t count;    /* +5 frame count (unaligned) */
} frontlight_anim_t;

/* --------------------------------------------------------- VanMoof prototypes */
/* led.c */
void frontlight_pwm_set_duty(unsigned int pct);                                       /* 0x0000040c */
void frontlight_led_channel_write(int channel, unsigned int data, unsigned int extra);/* 0x000027f8 */
void frontlight_led_set_brightness(unsigned char channel, unsigned int brightness);   /* 0x0000582e */
/* render.c */
int  frontlight_render_frame(const frontlight_anim_t *anim);                          /* 0x000028f8 */
int  frontlight_frame_table_store(int unused1, int unused2, const unsigned short *rec); /* 0x000031a4 (record ptr in 3rd arg) */
/* can.c */
void frontlight_log_event(uint32_t src_tag, uint16_t code, int argc, ...);            /* 0x0000313c */
int  frontlight_od_signal_register(uint32_t *spec, uint8_t *out_devdata);             /* 0x00005b3e */

#endif /* FRONTLIGHT_H */
