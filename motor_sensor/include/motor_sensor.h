#ifndef MOTOR_SENSOR_H
#define MOTOR_SENSOR_H

/*
 * motor_sensor.h — shared declarations for the VanMoof S5/A5 motor
 * position/current/temperature sensor ECU application layer.
 *
 * Reconstructed from the OEM image
 *   motor_sensor.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP Cortex-M4, LPC546xx-class, FreeRTOS; raw vector table, image base 0x0,
 * initial SP 0x20008000). Only VanMoof application code is reconstructed here;
 * the FreeRTOS kernel + Cortex-M port, the NXP LPC SDK/HAL (M_CAN, ADC, SCT,
 * GPIO/IOCON, clock, IAP-flash), the rotor-angle sensor IC driver and libgcc are
 * vendor (declared `extern`, satisfied upstream at link time).
 *
 * Behaviour-oriented: structs name the OEM byte offsets the code touches; the
 * decode was adversarially verified against the binary (its corrections folded
 * in, e.g. the first SYSCON write is 0x800, the 29-bit CAN-ID packing, and the
 * watchdog timer-start vs feed-callback split).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "compiler.h"

/* ------------------------------------------------------ LPC peripheral map */
#define SYSCON_BASE        0x40000000u
#define SYSCON_AHBCLKCTRL0 (SYSCON_BASE + 0x220u)   /* write-1-to-set clock gates */
#define SYSCON_AHBCLKCTRL1 (SYSCON_BASE + 0x240u)
#define SYSCON_DEVICE_ID0  (SYSCON_BASE + 0x2a0u)   /* clock-source probe */
#define SYSCON_MAINCLKSEL  (SYSCON_BASE + 0x280u)
#define SYSCON_AHBCLKDIV   (SYSCON_BASE + 0x30cu)
#define SYSCON_PERIPH_PD   (SYSCON_BASE + 0x38cu)   /* CAN reset/power */
#define MCAN_BASE          0x40006000u              /* M_CAN/FDCAN controller */
#define CAN_TIMING_BASE    0x400A0000u              /* CAN bit-timing/FIFO block (DAT_3b18) */
#define SCT_BASE           0x40008000u              /* SCT/CTimer (rotor capture) */
#define SPI2_BASE          0x40004000u
#define WWDT_BASE          0x4000C000u              /* window watchdog */
#define WWDT_FEED          (WWDT_BASE + 0x08u)
#define SENSOR_DATA_REG    0xf400u                  /* magnetic angle sensor data */
#define SENSOR_CFG_REG     0xfa00u                  /* sensor config register */

/* ARM Cortex-M system control */
#define SCB_ICSR           0xe000ed04u
#define SCB_ICSR_PENDSVSET 0x10000000u
#define SCB_SHPR3          0xe000ed20u
#define SYST_CSR           0xe000e010u
#define SYST_RVR           0xe000e014u
#define NVIC_ISER0         0xe000e100u

/* M_CAN register offsets (NXP layout) */
#define MCAN_SIDFC         0x88u   /* std ID filter config */
#define MCAN_XIDFC         0x8cu   /* ext ID filter config */
#define MCAN_GFC           0xa0u   /* global filter config */
#define MCAN_IR            0x50u   /* interrupt register */
#define MCAN_IE            0x54u   /* interrupt enable */
#define MCAN_TXBAR         0x5cu   /* TX buffer add request */
#define MCAN_TXBC          0xc0u   /* TX buffer config (msg-RAM start) */
#define MCAN_TXBTO         0xccu   /* TX buffer transmission occurred */
#define MCAN_TXBTIE        0xd0u   /* TX buffer transmission IRQ enable */
#define MCAN_NBTP          0x100u  /* nominal bit timing (via CAN_TIMING_BASE) */
#define MCAN_DBTP          0x104u  /* data bit timing */
#define MCAN_SSP           0x200u  /* transmitter delay comp */

/* ----------------------------------------------------------------- app types */

/* Generic function-pointer slot (vtable entries / stored callbacks). Casting a
 * concrete function-pointer type to/from this is allowed (function->function);
 * it avoids the ISO C void*<->function-pointer conversion. */
typedef void (*vfn_t)(void);

/* concrete vtable / callback signatures (cast from vfn_t at the call sites) */
typedef int  (*ms_probe_fn)(void *handle, uint32_t reg, uint32_t size, vfn_t self);
typedef int  (*ms_probe3_fn)(void *handle, uint32_t reg, uint32_t size);
typedef int  (*ms_fdwrite_fn)(void *handle, uint32_t reg, uint32_t size, void *buf,
                              void *a, uint32_t *b, uint32_t *c);
typedef void (*ms_rx_write_fn)(void *frame, int fd, int rsv);
typedef int  (*ms_rx_cb_fn)(void *ctx, void *a, void *b, uint32_t aux);
typedef void (*ms_handler3_fn)(void *ctx, void *data, uint32_t len);
typedef void (*ms_handler2_fn)(void *ctx, void *data);
typedef void (*ms_can_evt_cb)(uint32_t base, uint32_t *chan, uint32_t code,
                              uint32_t mask, uint32_t a, uint32_t *b);

/* The rotor-angle/current/voltage sensor MMIO register block (@0xf400). */
typedef struct sensor_regs {
    uint32_t ctrl;          /* +0x00 brake mode | operating mode */
    uint32_t current_u24;   /* +0x04 scaled current (5.75x ADC LSB) */
    uint32_t wdt_key;       /* +0x08 unlock: 0xAA then 0x55 */
    uint32_t status;        /* +0x0c polled != 0xFF for init-done */
    uint32_t _u10;          /* +0x10 */
    uint32_t adc_10bit;     /* +0x14 raw ADC (10-bit) */
    uint32_t voltage_u24;   /* +0x18 scaled voltage (1.5x ADC LSB) */
} sensor_regs_t;

/* The sensor IC access object (global @0x1301FE00) — vtable-dispatched driver. */
typedef struct sensor_obj {
    uint8_t  _pad00[0x10];
    vfn_t   *vtable;        /* +0x10 -> { [+0x10]=probe, [+0x14]=fd_write,
                             *           [+0x24]=classical_rx, [+0x30]=fd_rx } */
} sensor_obj_t;

/* A CAN/GATT subscription entry (stride 0x2c). */
typedef struct sub_entry {
    void    *frame_buf;     /* +0x00 frame data buffer (or CAN-ID/data) */
    uint32_t sem;           /* +0x04 TX semaphore / queue handle */
    uint32_t aux;           /* +0x08 callback arg3 */
    uint32_t frame_inline;  /* +0x0c CAN id / callback arg2 (inline) */
    uint8_t  state;         /* +0x10 0x02 = active */
    uint8_t  _pad11[0x0b];
    void    *timer;         /* +0x1c periodic re-send timer (brake frame) */
    vfn_t    rx_cb;         /* +0x14 ... (overlaps; see decode) */
    void    *sec_buf;       /* +0x20 secondary buffer */
    uint32_t sec_len;       /* +0x24 */
    void    *notify_q;      /* +0x28 notify queue (type-1) */
} sub_entry_t;
#define SUB_ENTRY_STRIDE   0x2cu

/* A flat subscription list. */
typedef struct subscription_list {
    uint32_t count;         /* +0x00 */
    uint32_t capacity;      /* +0x04 */
    uint8_t  _pad08[4];
    void    *data;          /* +0x0c flat entry array (0x2c stride) */
} subscription_list_t;

/* The byte ring buffer (ringbuf_read_consume / ringbuf_write_produce). */
typedef struct ring_buf {
    uint32_t read_ptr;      /* +0x00 */
    uint32_t write_ptr;     /* +0x04 */
    uint32_t capacity;      /* +0x08 */
    uint8_t  _pad0c[0x0c];
    uint8_t *buf;           /* +0x18 backing store */
} ring_buf_t;

/* M_CAN controller driver state (global @0x1301FE00 view). */
typedef struct can_ctrl_state {
    uint8_t  _pad00[6];
    uint8_t  config_mode;   /* +0x06 == 3 -> CAN-FD */
    uint8_t  _pad07[2];
    uint8_t  init_busy;     /* +0x09 */
    uint8_t  _pad0a[6];
    vfn_t   *vtable;        /* +0x10 { [+0x24]=classical_rx, [+0x30]=fd_rx } */
    uint8_t  _pad14[0x0d];
    uint8_t  tx_done_en;    /* +0x21 */
} can_ctrl_state_t;

/* The in-memory flash page buffer / sector writer. */
typedef struct flash_obj {
    uint8_t  _pad00[0x3c];
    uint8_t  initialized;   /* +0x3c */
    uint8_t  _pad3d[3];
    void    *page_buf;      /* +0x40 0x200-byte page buffer */
    uint16_t cur_sector;    /* +0x44 0xFFFF = flushing */
    int16_t  sectors_left;  /* +0x46 erase countdown */
} flash_obj_t;

typedef struct flash_ctx {
    uint8_t  _pad00[6];
    uint16_t result_start;  /* +0x06 */
    uint16_t in_progress;   /* +0x08 */
    uint8_t  _pad0a[6];
    uint32_t status;        /* +0x10 */
    uint8_t  _pad14;
    uint8_t  flag15;        /* +0x15 */
    uint8_t  _pad16[2];
    flash_obj_t *obj;       /* +0x18 */
} flash_ctx_t;

/* SPI wire / CAN-notify frame: 13 bytes (shared with the bridge family). */
#define MS_FRAME_FLAG      0x10
#define MS_FRAME_LEN       13

/* -------------------------------------------------- vendor callees (deferred) */
/* FreeRTOS kernel + Cortex-M port */
extern int      xTaskCreate(void (*fn)(void *), const char *name, uint16_t stack,
                            void *arg, uint32_t prio, void **handle);            /* 0x29ec */
extern void    *pvPortMalloc(size_t n);                                          /* 0x27f4 (heap_malloc) */
extern void    *xTimerCreate_wrap(void *block, uint32_t period, int reload,
                                  void *id, void (*cb)(void *));                 /* 0x2c34 */
extern int      freertos_queue_send_blocking(void *q, void *item, uint32_t to);  /* vendor */
extern int      freertos_queue_send_from_isr_or_task(void *q);                   /* vendor */
extern int      freertos_stream_buffer_send_blocking(uint32_t ch, void *buf, uint32_t len, uint32_t to); /* vendor */
extern void     freertos_timer_generic_command(void *t);                         /* vendor */
extern void     vTaskEnterCritical(void);                                        /* vendor */
extern void     vTaskExitCritical(void);                                         /* vendor */
extern uint32_t port_set_interrupt_mask(void);                                   /* vendor (also assert) */
extern void     port_clear_interrupt_mask(uint32_t m);                           /* vendor */
extern void     port_yield_pend_sv(void);                                        /* vendor */
extern void     vPortStartFirstTask(void);                                       /* vendor */

/* NXP LPC SDK/HAL + sensor driver */
extern void     gpio_set_pin(int port, int pin, const void *cfg);                /* 0x0a5c */
extern void     NVIC_EnableIRQ(int irq);                                         /* 0x03a8 */
extern void     clock_prescaler_config(int sel);                                 /* 0x4d92 */
extern void     can_bus_error_recover(void);                                     /* 0x0b28 */
extern void     mcan_lowlevel_init(void);                                        /* 0x15fc (NXP M_CAN init) */
extern void     can_mailbox_buffer_setup(uint32_t base, uint32_t ev);            /* vendor */
extern int      subscription_find_entry_or_null(void *ctx, void *key, void *a, void *b, void *c); /* 0x4d3e */
extern void     can_tx_frame_write_chunks(void *list, void *frame);              /* 0x50cc */
extern int      can_tx_poke(void *handle, uint32_t reg);                         /* 0x0aec */
extern int      can_tx_peek(void *handle);                                       /* 0x0abc */
extern int      can_tx_dispatch(void *handle, uint32_t reg, uint32_t size, void *out); /* 0x0790 */
extern int      can_rx_dequeue(void);                                            /* 0x0a28 */
extern void     can_tx_disable(void *chan);                                      /* 0x4d56 */
extern int      can_is_fd_mode(void *ctrl);                                      /* 0x0754 */
extern int      queue_send_item(void *q, void *item, uint32_t to);               /* 0x1b10 */
extern int      flash_page_verify_and_advance(flash_obj_t *obj);                 /* 0x5740 */
extern int      return_zero_stub(void);                                          /* board-rev GPIO index */
extern void     gpio_pulse_wait(uint32_t arg);                                   /* GPIO deglitch wait */
extern int      strncmp_(const void *a, const void *b, size_t n);                /* 0x5f72 */

/* libc */
extern void    *mem_cpy(void *d, const void *s, size_t n);                       /* 0x5f48 (memcpy) */
extern void    *mem_set(void *d, int c, size_t n);                               /* 0x5f62 (memset) */

/* ----------------------------------------------------------- app prototypes */
/* init (main.c) */
void     motor_sensor_hw_init_and_scheduler_start(void);                          /* 0x375c */
uint32_t peripheral_hw_init(void);                                                /* 0x229e */
uint32_t can_controller_init(uint32_t a, uint32_t b);                             /* 0x2d64 */

/* tasks (tasks.c) */
void     motor_sensor_task_loop(void *ctx, uint32_t p2, uint32_t p3, uint32_t period_ms); /* 0x3382 */
void     can_tx_manager_loop(void *p1, uint32_t p2, int msg_code, void *handler);  /* 0x1d5c */
void     can_tx_task(uint32_t *chan);                                              /* 0x17ec */
void     can_event_fsm_dispatch(uint32_t mcan_base, uint32_t *chan);              /* 0x1508 */

/* sensor (sensor.c) */
int      sensor_reg_block_write(void *handle, uint32_t reg_addr, void *buf);      /* 0x3170 */
int      sensor_config_write(void *handle, void *buf, void *p3);                  /* 0x3204 */
int      sensor_position_read(void *handle, uint8_t out_buf[12]);                 /* 0x3284 */
uint32_t sensor_speed_sample(void);                                               /* 0x3310 */

/* CAN report frames (can_report.c) */
void     can_send_status_frame_382(uint32_t *ctx, uint32_t a, uint32_t b);        /* 0x2580 */
void     can_send_data_frame_8887(uint32_t a, uint32_t *data, uint32_t c, uint32_t **ctx); /* 0x25ee */
uint32_t can_send_brake_status_4a1(uint32_t a, uint32_t b, uint8_t *brake, uint32_t d); /* 0x2dd4 */
void     can_send_status_frame_2a1(uint32_t a, uint32_t b, uint32_t c);           /* 0x26bc */
uint32_t can_frame_pack_and_enqueue(void *ctx, uint8_t *raw_frame);              /* 0x544a */
void     can_diag_log_send(uint32_t opcode, uint16_t event, int argc, ...);       /* 0x2e80 */

/* CAN I/O (can_io.c) */
uint32_t can_tx_submit_frame(void *ctx, uint32_t *frame, uint32_t p3);            /* 0x59fa */
uint32_t can_tx_buf_write(void *obj, uint32_t p2, const void *src, int len);      /* 0x5a70 */
void     can_frame_receive_and_dispatch(void *ctx, uint8_t *frame_id_block);      /* 0x599a */
uint32_t can_frame_notify_subscribers(void *ctx, void *frame_desc);              /* 0x5914 */
void     can_rx_enqueue(void *rx_frame);                                          /* 0x0a04 */
int      can_tx_send_with_flow_control(uint32_t *conn, uint32_t param2, int is_isr); /* 0x21a8 */

/* flash + misc (flash.c / util.c) */
uint32_t flash_write_sector(flash_obj_t *obj, uint32_t off, const void *src, uint32_t len); /* 0x2724 */
int32_t  flash_erase_and_program_sector(flash_ctx_t *ctx, uint32_t p2, uint16_t *spec, uint32_t p4); /* 0x5882 */
void     watchdog_timer_start(void *xTimer, uint32_t p2, uint32_t p3);            /* 0x2d14 */
int32_t  gatt_attr_write_dispatch(void *ctx, void *req);                          /* 0x2ee8 */
int32_t  subscription_list_add(subscription_list_t *list, const void *entry, uint32_t p3, uint32_t p4); /* 0x52d2 */
uint32_t ringbuf_write_produce(ring_buf_t *rb, const uint8_t *src, uint32_t len);  /* 0x5278 */
uint32_t ringbuf_read_consume(ring_buf_t *rb, uint8_t *dst, uint32_t avail, uint32_t req); /* 0x520a */

#endif /* MOTOR_SENSOR_H */
