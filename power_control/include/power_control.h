#ifndef POWER_CONTROL_H
#define POWER_CONTROL_H

/*
 * power_control.h — shared declarations for the VanMoof S5/A5 power-management
 * sub-ECU application layer.
 *
 * Reconstructed from the OEM image
 *   power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial SP
 * 0x20008000). Only VanMoof application code is reconstructed here; the FreeRTOS
 * kernel + Cortex-M port, the NXP SDK/HAL (eDMA, M_CAN, peripherals) and libgcc
 * are vendor (declared `extern` and satisfied upstream at link time).
 *
 * RAM globals are written as literal-address volatile accesses so the emitted
 * code matches the OEM (which loads the absolute address from a literal pool).
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ------------------------------------------------------------------ globals */

/* NTC/thermistor calibration table: 166 monotonically-decreasing uint16 entries
 * in flash @0x7c00 (beyond the loaded code segment). // 0x00007c00 */
extern const uint16_t g_ntc_temp_table[166];

/* Live rail/flag state block (peripheral-mapped, byte-addressed). // 0x4008c000 */
#define POWER_STATE          (0x4008c000u)
/* Power-control / CAN messaging context (mode handle, rail ctrl @+0x20). // 0x20000664 */
#define POWER_CAN_CTX        (0x20000664u)
/* Most-recent ADC sample block (+4 = uint16 sample). // 0x20000658 */
#define POWER_ADC_STATE      (0x20000658u)
/* Capabilities/limit negotiation context (+0x16/+0x18 = advertised maxima). // 0x20000770 */
#define POWER_CAP_CTX        (0x20000770u)
/* Status-report mirror byte. // 0x20000088 */
#define POWER_STATUS_MIRROR  (0x20000088u)
/* Event subsystem context pointer (NULL until initialised). // 0x200009f8 */
#define POWER_EVENT_CTX      (0x200009f8u)

/* ----------------------------------------------- vendor callees (deferred) */
/* FreeRTOS kernel + Cortex-M port */
extern void    *pvPortMalloc(size_t n);                                                    /* vendor freertos // 0x00002030 (heap_4) */
extern uint32_t rtos_task_notify_wait(uint32_t handle, uint32_t clr_in, uint32_t clr_out, uint32_t ticks);  /* vendor freertos // 0x00001844 (xTaskNotifyWait) */
extern int      rtos_queue_receive(uint32_t queue, void *buf, uint32_t ticks);             /* vendor freertos // 0x00001a28 (xQueueReceive) */
extern void     rtos_task_delay(uint32_t ms);                                              /* vendor freertos // 0x0000158c (vTaskDelay) */
extern void     vPortEnterCritical(void);                                                  /* vendor freertos // 0x00000db8 */
extern void     vPortExitCritical(void);                                                   /* vendor freertos // 0x00000dd4 */
extern void     power_panic(void);                                                         /* vendor freertos // 0x00005e68 (configASSERT trap) */
extern void     event_queue_publish(uint32_t queue, uint32_t desc, uint32_t flags, void *rec, uint32_t len);  /* vendor freertos // 0x00006bd0 (message-buffer send) */
extern int      comms_wait(uint32_t wait_handle, int ticks);                               /* vendor freertos // 0x00006170 (queue/sem take) */
extern void     comms_release(uint32_t wait_handle);                                       /* vendor freertos // 0x000068be (queue/sem give) */
/* NXP SDK/HAL — comms registry + M_CAN / flash drivers */
extern void    *comms_registry_lookup(uint32_t handle, uint32_t descriptor, void *payload, uint32_t flags);  /* vendor hal // 0x00005c82 */
extern void     can_event_dispatch(uint32_t channel, int desc, void *rec, uint32_t marker);                 /* vendor hal // 0x000060b0 (multi-frame fragmenter) */
extern void     can_tx_abort(uint32_t instance);                                           /* vendor hal // 0x00005c9a (M_CAN TXBCR/CCCR) */
extern int      storage_dev_prepare(int dev);                                              /* vendor hal // 0x00001dc4 (flush pending page) */
extern int      storage_dev_erase_sector(int dev, uint32_t addr);                          /* vendor hal // 0x0000100c (erase one sector) */
extern int      power_watchdog_timer_create(void *timer, uint32_t period_ms, uint32_t ctx, void *cb);  /* vendor hal // 0x00003658 (xTimerCreate+start) */
/* libgcc / compiler runtime */
extern void     mem_set(void *d, int c, uint32_t n);                                       /* vendor libgcc // 0x00006ec8 (memset) */
extern void     mem_copy(void *d, const void *s, uint32_t n);                              /* vendor libgcc // 0x00006eae (memcpy) */

/* --------------------------------------------------------- VanMoof prototypes */
int      adc_to_temperature_lookup(void);                                                  /* 0x00000e9c */
int      power_send_mode_command(uint32_t opcode);                                         /* 0x00003200 */
void     power_send_capabilities_frame(uint32_t req_a, uint32_t req_b);                    /* 0x00003234 */
void     power_build_status_report(uint32_t handle, uint32_t seq, uint32_t arg3);          /* 0x00003730 */
void     power_log_event(uint32_t src_tag, uint16_t code, int argc, ...);                  /* 0x00003980 */
int      power_rail_enable_sequence(int settle_ms);                                        /* 0x00003b60 */
int      power_set_mode(uint32_t mode);                                                    /* 0x00003c94 */
int      storage_erase_program_sectors(int ctx, uint32_t unused2, const uint16_t *req, uint32_t unused4);  /* 0x000065b8 */
uint32_t can_build_event_record(int instance, int desc);                                   /* 0x0000664a */
int      can_request_obj_1a4(uint32_t handle, uint8_t **out);                              /* 0x00006226 */
void     can_request_obj_10a7(uint32_t handle, uint8_t **out);                             /* 0x00006260 */
void     can_tx_task(uint32_t *instance);                                                  /* 0x00006292 */
int      can_request_response_8808(uint32_t handle, uint32_t *payload);                    /* 0x000069d4 */
int      can_send_obj_1a4(uint32_t handle);                                                /* 0x00006a48 */
int      can_send_obj_10a7(uint32_t handle);                                               /* 0x00006a84 */

#endif /* POWER_CONTROL_H */
