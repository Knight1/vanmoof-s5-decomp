#ifndef POWER_PEDAL_H
#define POWER_PEDAL_H

/*
 * power_pedal.h — shared declarations for the VanMoof S5/A5 pedal-assist /
 * torque + cadence sensor sub-ECU application layer.
 *
 * Reconstructed from the OEM image
 *   power_pedal.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC546xx Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). CAN node 0x92 / on-wire 0xA2.
 *
 * power_pedal is a sensor: it samples pedal torque + cadence in the NXP HAL/ADC
 * front-end and PUBLISHES them as CAN object-dictionary (OD) signals; the assist
 * curve is computed downstream (motor_control). Its VanMoof application layer is
 * therefore the shared CAN-OD signal + event-log infrastructure — the same
 * primitives carved for elock/eshifter/frontlight/rearlight. The pedal sampling,
 * the M_CAN / ADC / SCTimer drivers, FreeRTOS and libgcc are vendor.
 *
 * RAM/MMIO globals are named `#define`s, accessed as literal-address volatile
 * casts so the emitted code matches the OEM.
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ---------------------------------------------------- device addresses ---- */
/* RAM globals */
#define PP_LOG_CTX            0x20000924u   /* ptr-to-ptr: event-publisher context */
/* flash tables / constants */
#define PP_LOG_TAG            0x00003941u   /* message-buffer handle/tag for log records */

/* ----------------------------------------------- vendor callees (deferred) */
extern void  mem_set(void *dst, int c, uint32_t n);                                   /* vendor libgcc // 0x00006cf8 (memset) */
extern int   mem_compare(const void *a, const void *b, uint32_t n);                   /* vendor libgcc // 0x00006cbe (memcmp) */
extern void  message_buffer_send(void *mb, void *handle, uint32_t flags, void *data, uint32_t len); /* vendor freertos // 0x00006728 */
extern int   nvm_record_open(void);                                                   /* vendor hal // 0x00001fa8 */
extern int   nvm_record_read(int handle, void *buf, uint32_t blob_id, uint32_t len);  /* vendor hal // 0x00000a88 */
extern void  nvm_record_release(int handle);                                          /* vendor hal // 0x00005c7a */
extern void *od_registry_lookup(uint32_t registry, uint32_t descriptor);              /* vendor hal // 0x000059fe */
extern int   od_resource_wait(void *res, int timeout);                                /* vendor hal // 0x00005e72 */
extern void  od_signal_release(void *res);                                            /* vendor hal // 0x000064a2 (OD resource release) */
extern int   od_signal_send(void *bus, void *node);                                   /* vendor hal // 0x00006226 */

/* --------------------------------------------------------- VanMoof prototypes */
/* can.c */
void power_pedal_log_event(uint32_t src_tag, uint16_t code, int argc, ...);           /* 0x00003cb4 */
int  power_pedal_od_signal_register(const uint8_t *desc_id, void *out_buf);            /* 0x00006978 (descriptor 0x87, NVM blob 0x30) */
int  power_pedal_od_signal_register_91(const uint8_t *desc_id, void *out_buf);         /* 0x00006a78 (descriptor 0x91, NVM blob 0x40) */
int  power_pedal_od_signal_8808_send(void *bus, const uint32_t *payload);             /* 0x000065b8 (descriptor 0x8808) */

#endif /* POWER_PEDAL_H */
