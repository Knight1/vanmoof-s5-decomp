#ifndef ESHIFTER_H
#define ESHIFTER_H

/*
 * eshifter.h — shared declarations for the VanMoof S5/A5 electronic gear-shifter
 * sub-ECU application layer.
 *
 * Reconstructed from the OEM image
 *   eshifter.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC546xx Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). Only VanMoof application code is reconstructed here; the
 * FreeRTOS kernel + Cortex-M port, the NXP SDK/HAL (SCTimer/PWM, M_CAN, ADC,
 * Flexcomm, flash) and libgcc are vendor (declared `extern` and satisfied
 * upstream at link time).
 *
 * RAM globals are written as literal-address volatile accesses so the emitted
 * code matches the OEM (which loads the absolute address from a literal pool).
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ---------------------------------------------------- device addresses ----
 * Named MMIO peripheral bases and RAM globals. These are plain literal-address
 * constants — substituting the name for the hex changes nothing in the emitted
 * code (the compiler folds them identically), it only makes the reconstruction
 * readable. Addresses are the OEM absolute values. */

/* MMIO peripherals */
#define ESH_MOTOR_BASE        0x4008c000u   /* lock/gear actuator motor driver block (+2/+3/+0x19 enable/dir) */
#define ESH_PWM_BASE          0x40028000u   /* SCTimer/PWM block (+0x7c duty) */
#define ESH_ADC_BASE          0x400a0000u   /* position-sensor ADC (+0x300 sample register) */

/* RAM globals — position/drive control */
#define ESH_REF_SAMPLES       0x200001b8u   /* 4x signed reference samples / averaged result table */
#define ESH_MOTOR_CMD         0x200001ccu   /* motor torque/current command output */
#define ESH_DELTA_RING        0x200001d0u   /* 128-entry signed position-delta ring buffer */
#define ESH_DELTA_RING_IDX    0x20000f88u   /* position-delta ring write index */
#define ESH_IIR_FILT_RING     0x2000049cu   /* 128-entry float IIR-filter ring buffer */
#define ESH_POS_ACCUM         0x200006c4u   /* signed position accumulator */
#define ESH_POS_DELTA_PAIR    0x200006c8u   /* [cur delta, prev delta] pair */
#define ESH_ACTUATOR_PTR      0x200006a0u   /* pointer to actuator state struct */
#define ESH_ACTUATOR_LASTPOS  0x200006d8u   /* actuator last-position snapshot */
#define ESH_CONFIG_REC_PTR    0x2000069cu   /* pointer to calibration config record */
#define ESH_POS_LAST14        0x20000f68u   /* last 14-bit electrical position */
#define ESH_POS_RAW_PREV      0x20000f78u   /* prior raw position */
#define ESH_POS_RAW_CUR       0x20000f7au   /* current raw position */
#define ESH_DRIVE_RAMP        0x20000f7cu   /* drive ramp/dwell counter + stop flag */
#define ESH_DIR_FLAG_REV      0x20000f7eu   /* reverse-direction phase flag */
#define ESH_DIR_FLAG_FWD      0x20000f80u   /* forward-direction phase flag */
#define ESH_POS_WRAP_IDX      0x20000f8au   /* position-wrap index (clamped 0..14) */

/* RAM globals — time/tick */
#define ESH_TIME_COARSE       0x20000758u   /* 32-bit coarse time / wrap counter */
#define ESH_TIME_BASE         0x20000e54u   /* 32-bit base time value */
#define ESH_TIME_FINE         0x20000f76u   /* 16-bit fine time term (feeds calc_time_offset) */

/* RAM globals — IIR filter */
#define ESH_IIR_INPUT_RING    0x20000e68u   /* 128-entry u16 IIR input ring */
#define ESH_IIR_RING_IDX      0x20000f82u   /* IIR ring write index */

/* RAM globals — calibration */
#define ESH_CALIB_ACC_A       0x2000010cu   /* A-side calibration accumulator table */
#define ESH_CALIB_ACC_B       0x2000011cu   /* B-side calibration accumulator table */
#define ESH_CALIB_MAX_A       0x20000130u   /* A-side per-channel max tracker */
#define ESH_CALIB_MIN_B       0x20000140u   /* B-side per-channel min tracker */
#define ESH_CALIB_FSM         0x20000158u   /* calibration FSM state byte */
#define ESH_CALIB_COUNT_A     0x20000e58u   /* A-side calibration counters */
#define ESH_CALIB_COUNT_B     0x20000e60u   /* B-side calibration counters / A-loop end */
#define ESH_CALIB_ENABLE      0x20000f89u   /* calibration enable byte */
#define ESH_SAMPLE_COPY       0x20000f6au   /* live-sample copy destination */

/* RAM globals + constants — event log / CAN-OD signalling */
#define ESH_LOG_CTX           0x2000077cu   /* ptr-to-ptr event-publisher context */
#define ESH_LOG_MSG_HANDLE    0x387du       /* message-buffer handle for log records */
#define ESH_LOG_TAG           0x3a36c02au   /* source tag for eshifter log events */
#define ESH_VMFW_MAGIC        0x57464d56u   /* 'VMFW' frame magic (position-frame tag) */

/* Calibration record magic */
#define ESH_CALIB_MAGIC       0x8550u       /* valid-calibration marker (config record +0x88) */

/* ------------------------------------------------------------------ types */
/* CAN Object-Dictionary message descriptor (only the touched fields named). */
typedef struct od_msg_desc {
    uint32_t _r0[3];        /* +0x00 */
    uint32_t can_id;        /* +0x0c (low 3 bytes = 24-bit CAN id) */
    uint8_t  mode;          /* +0x10 0=local 1/2=multi-frame */
    uint8_t  _r1[0x0f];     /* +0x11 */
    void    *payload;       /* +0x20 single-frame payload pointer */
    uint32_t payload_len;   /* +0x24 single-frame payload length */
} od_msg_desc_t;

/* local OD send vtable slot at ctx+0x5a4 */
typedef void (*od_send_fn)(void *bus, const void *rec);

/* 0x51 OD position-sample frame (read into a 0xfe/0x100-byte buffer). */
typedef struct eshifter_pos_frame {
    uint32_t magic;        /* +0 'VMFW'-tag word, compared against 0x3a36c02a */
    uint8_t  frame_type;   /* +4 */
    uint8_t  subcode;      /* +5 */
    int16_t  samples[125]; /* +6 (sentinel 0xffff = empty slot) */
} eshifter_pos_frame_t;

/* ----------------------------------------------- vendor callees (deferred) */
/* CAN Object-Dictionary comms-registry middleware (generic, shared across sub-ECUs) */
extern void *od_registry_lookup(uint32_t registry, uint32_t descriptor);             /* vendor hal // 0x000059e0 */
extern int   od_registry_wait(void *lock, int ticks);                                /* vendor hal // 0x00005f10 */
extern void  od_registry_release(void *lock);                                        /* vendor hal // 0x0000656e */
extern void  can_send_multiframe(void *bus, const od_msg_desc_t *desc, void *frame, int len); /* vendor hal // 0x00005e50 */
extern int   can_send_multiframe_ctx(void *nvm, const void *rec, uint32_t len, int off);      /* vendor hal // 0x00006e8e (eshifter NVM/OD seam) */
/* NXP flash / NVM page driver + OD descriptor read */
extern void *nvm_record_alloc(void);                                                 /* vendor hal // 0x00002f50 */
extern int   nvm_record_read(void *nvm, void *dst, uint32_t rec_id, uint32_t len);   /* vendor hal // 0x00000a20 (page-record read) */
extern int   od_descriptor_read(void *dev, uint32_t desc, uint8_t *status, void *buf, uint32_t maxlen); /* vendor hal // 0x0000632a (OD descriptor read) */
extern int   nvm_record_block_write(void *dev, uint32_t desc, uint32_t off, void *buf, uint32_t len, uint32_t verify); /* vendor hal // 0x00006822 */
extern int   nvm_record_block_write_verify(void *record, uint32_t reg, uint32_t index, const void *src, uint32_t len); /* vendor hal // 0x000023c4 */
extern int   nvm_record_commit(void *nvm, int flag);                                 /* vendor hal // 0x00002f98 */
extern void  nvm_record_free(void *nvm);                                             /* vendor hal // 0x00005c4a */
/* FreeRTOS + libgcc */
extern void  message_buffer_send(void *mb, void *handle, uint32_t flags, void *data, uint32_t len); /* vendor freertos // 0x0000660a */
extern void *pvPortMalloc(uint32_t size);                                            /* vendor freertos // 0x00002e1c */
extern void  mem_set(void *dst, int c, uint32_t n);                                  /* vendor libgcc // 0x000070f2 (memset) */
extern void  mem_copy(void *dst, const void *src, uint32_t n);                       /* vendor libgcc // 0x000070d8 (memcpy) */
extern int   mem_compare(const void *a, const void *b, uint32_t n);                  /* vendor libgcc // 0x000070b8 (memcmp) */
extern float fabsf(float x);                                                         /* vendor libgcc // 0x00005918 */

/* --- VanMoof callees not carved this batch (forward decls) ---
 * The classifier bucketed these vendor-hal, but the gear-FSM call sites use them
 * with app-specific semantics; left extern (satisfied at link), flagged for a
 * later carve/review. */
unsigned long long eshifter_tick_update(void);            /* 0x000003c0 (time/tick helper) */
void               eshifter_calib_reset(void);            /* 0x0000046c (calibration reset) */
void               eshifter_actuator_dead_stop(unsigned char *state); /* 0x000004c8 (park actuator) */

/* --------------------------------------------------------- VanMoof prototypes */
/* gear.c */
void eshifter_sensor_pair_average(int cfg);                                           /* 0x00000420 */
int  eshifter_calc_time_offset(void);                                                 /* 0x00000444 */
void eshifter_actuator_drive_step(unsigned char *state, int target);                 /* 0x000005f0 */
void eshifter_position_sensor_task_step(void);                                        /* 0x000025a4 */
/* can.c */
void eshifter_log_event(uint32_t src_tag, uint16_t code, int argc, ...);             /* 0x0000332c */
int  eshifter_od_signal_send_51(void *dev, uint32_t offset, uint16_t *buf, uint32_t len); /* 0x00003988 */
int  eshifter_read_position_status(uint32_t *ctx);                                    /* 0x000039ec */
int  eshifter_send_position_signal(int ctx, int position);                            /* 0x0000700e */
int  eshifter_encode_status_frame(void *ctx, const od_msg_desc_t *desc);             /* 0x00006a74 */
int  eshifter_od_signal_8808_send(void *ctx, const uint32_t payload[2]);             /* 0x00006afa */
int  eshifter_od_id_exchange(const uint32_t *arg, void *out_id14);                   /* 0x00006ca4 */
int  eshifter_od_send_signal_13b(const uint32_t *arg, uint32_t unused, const void *payload13); /* 0x00006f0c */
/* config.c */
void eshifter_write_config_record(uint8_t *record);                                  /* 0x000068c0 */
void eshifter_emit_vmfw_header(void *ctx, void *buf);                                 /* 0x00006fd0 */

#endif /* ESHIFTER_H */
