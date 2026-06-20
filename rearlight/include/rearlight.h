#ifndef REARLIGHT_H
#define REARLIGHT_H

/*
 * rearlight.h — shared declarations for the VanMoof S5/A5 rear LED light
 * controller application layer.
 *
 * Reconstructed from the OEM image
 *   rearlight.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC546xx Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). Same firmware family as the frontlight (node 0x87); the LED
 * application functions are structurally identical with shifted addresses, but
 * the rearlight drives 20 LEDs (frontlight 16), adds a channel-remap step, and
 * has no separate pwm_set_duty. FreeRTOS, the NXP SDK/HAL and libgcc are vendor.
 *
 * RAM/MMIO globals are named `#define`s, accessed as literal-address volatile
 * casts so the emitted code matches the OEM.
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ---------------------------------------------------- device addresses ---- */
/* RAM globals */
#define RL_LED_CB             0x20000080u   /* LED driver control block (u32[] cb[0..0xf]) */
#define RL_FRAME_QUEUE        0x2000006cu   /* ptr to FreeRTOS frame queue/sem handle */
#define RL_REMAP_ENABLE       0x20001694u   /* u8: non-zero enables the channel permutation */
#define RL_MASTER_BRIGHTNESS  0x2000000cu   /* u8 master brightness (0xff = full) */
#define RL_FRAME_TABLE        0x20000874u   /* primary 300-entry frame table (RAM, 12 B/entry) */
#define RL_LOG_CTX            0x20000190u   /* ptr-to-ptr: event-publisher context */
/* flash tables / constants */
#define RL_GAMMA_LUT          0x00007835u   /* 256-entry u8 gamma LUT (flash, beyond loaded image) */
#define RL_FRAME_TABLE_WRAP   0x00006660u   /* secondary/wrap frame table (flash) */
#define RL_LED_CALLBACK       0x000015e0u   /* LED/SCTimer driver completion callback (cb[7]) */
#define RL_LOG_TAG            0x000039b9u   /* message-buffer handle/tag for log records */
#define RL_LOG_SRC_OVERFLOW   0xe2ed434fu   /* log src_tag for frame-table overflow (code 0xb2) */

/* ----------------------------------------------- vendor callees (deferred) */
extern void  mem_set(void *dst, int c, uint32_t n);                                   /* vendor libgcc // 0x000059a6 (memset) */
extern void  mem_copy(void *dst, const void *src, uint32_t n);                        /* vendor libgcc // 0x0000598c (memcpy) */
extern int   mem_compare(const void *a, const void *b, uint32_t n);                   /* vendor libgcc // 0x0000596c (memcmp) */
extern void  message_buffer_send(void *mb, void *handle, uint32_t flags, void *data, uint32_t len); /* vendor freertos // 0x00005410 */
extern int   rtos_sem_take(uint32_t handle, uint32_t ticks);                          /* vendor freertos // 0x00001268 (1=signalled) */
extern void  rtos_sem_give(uint32_t handle, int x);                                   /* vendor freertos // 0x0000210c */
extern void  od_frame_marker(uint32_t marker);                                        /* vendor hal // 0x00002728 (OD begin/end-frame) */
extern void  od_signal_release(void *res);                                            /* vendor hal // 0x00005364 (OD resource release) */
extern int   nvm_record_open(void);                                                   /* vendor hal // 0x00001adc */
extern int   nvm_record_read(int handle, void *buf, uint32_t len);                    /* vendor hal // 0x0000492c */
extern void  nvm_record_release(int handle);                                          /* vendor hal // 0x00004a90 */
extern void *od_registry_lookup(uint32_t registry, uint32_t descriptor);              /* vendor hal // 0x0000487a */
extern int   od_resource_wait(void *res, int timeout);                                /* vendor hal // 0x00004cca */
extern int   od_signal_send(void *bus, void *node);                                   /* vendor hal // 0x00005046 */

/* ------------------------------------------------------------------ types */
/* render argument (anim/window descriptor); unaligned u16 fields match the OEM. */
typedef struct __attribute__((packed)) {
    uint8_t  _pad0[3]; /* +0 */
    uint16_t start;    /* +3 first frame index (unaligned) */
    uint16_t count;    /* +5 frame count (unaligned) */
} rl_anim_request_t;

/* --------------------------------------------------------- VanMoof prototypes */
/* led.c */
void rearlight_led_channel_write(uint32_t channel, uint32_t data);                    /* 0x00002698 */
void rearlight_led_set_brightness(uint8_t channel, uint8_t brightness);               /* 0x00005318 */
/* render.c */
int  rearlight_render_frame(const rl_anim_request_t *req);                            /* 0x00002798 */
int  rearlight_frame_table_store(uint32_t unused1, uint32_t unused2, const uint8_t *rec); /* 0x000030e8 */
/* can.c */
void rearlight_log_event(uint32_t src_tag, uint16_t code, int argc, ...);             /* 0x00003080 */
int  rearlight_od_signal_register(const uint8_t *desc_id, void *out_buf);             /* 0x00005628 (descriptor 0x87) */
int  rearlight_od_signal_register_91(const uint8_t *desc_id, void *out_buf);          /* 0x0000572a (descriptor 0x91) */
int  rearlight_od_signal_8808_send(void *bus, const uint32_t *payload);               /* 0x0000539c (descriptor 0x8808) */

#endif /* REARLIGHT_H */
