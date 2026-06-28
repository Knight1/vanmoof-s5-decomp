#ifndef IMX8_BRIDGE_H
#define IMX8_BRIDGE_H

/*
 * imx8_bridge.h — shared declarations for the VanMoof S5/A5 SPI↔CAN-FD bridge
 * co-processor application layer.
 *
 * Reconstructed from the OEM image
 *   imx8_bridge.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (NXP LPC55S69 Cortex-M4F, FreeRTOS; raw vector table, image base 0x0, initial
 * SP 0x20008000). Only VanMoof application code is reconstructed here; the
 * FreeRTOS kernel + Cortex-M port, the NXP LPC55 SDK/HAL (SYSCON/IOCON/DMA/
 * dual M_CAN CAN-FD / FlexComm SPI-slave / IAP-flash / CLOCK_*) and libgcc are
 * vendor (declared `extern` and satisfied upstream at link time).
 *
 * Reconstruction is behaviour-oriented (the OEM is stripped C++/C over the SDK):
 * structs name the OEM byte offsets that the code actually touches; RAM globals
 * are written as literal-address accesses so the emitted loads match the OEM.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "compiler.h"

/* ------------------------------------------------------ LPC55S69 peripherals */
#define SYSCON_BASE        0x40000000u
#define SYSCON_DEVICE_ID0  (SYSCON_BASE + 0x2a0u)   /* 0=main,1=xtal,2=PLL0 */
#define SYSCON_AHBCLKCTRL0 (SYSCON_BASE + 0x220u)   /* peripheral clock enables */
#define SYSCON_AHBCLKDIV   (SYSCON_BASE + 0x38cu)
#define IOCON_BASE         0x40001000u
#define DMA0_BASE          0x40004000u
#define GPIO_BASE          0x40004000u

/* ARM Cortex-M system control */
#define SCB_ICSR           0xe000ed04u
#define SCB_ICSR_PENDSVSET 0x10000000u              /* bit 28 */
#define SYST_CSR           0xe000e010u
#define SYST_RVR           0xe000e014u
#define SYST_CVR           0xe000e018u
#define SCB_SHPR3          0xe000ed20u

/* ----------------------------------------------------------------- app types */

/*
 * spi_channel_t — a pointer-ring SPI channel object (spi_channel_alloc 0x5cc0).
 * Header is 0x48 bytes; the ring data follows inline at +0x48 (elem_size != 0).
 */
typedef struct spi_channel {
    uint8_t  *data_ptr;        /* +0x00 ring data start                         */
    uint8_t  *write_ptr;       /* +0x04 write cursor                            */
    uint8_t  *end_ptr;         /* +0x08 one-past-end (data_ptr+cap*elem)        */
    uint8_t  *last_ptr;        /* +0x0c last valid slot (end-elem)              */
    uint32_t  tx_list[5];      /* +0x10 FreeRTOS List_t (TX waiters)            */
    uint32_t  rx_list[5];      /* +0x24 FreeRTOS List_t (RX waiters)            */
    uint32_t  current_count;   /* +0x38 items held                              */
    uint32_t  capacity;        /* +0x3c max items                               */
    uint32_t  elem_size;       /* +0x40 bytes/item (also pending-tx sentinel)   */
    uint8_t   seq_a;           /* +0x44 sequence byte A (0xff = reset)          */
    uint8_t   tx_seq;          /* +0x45 TX sequence byte (0xff = reset)         */
    uint8_t   _pad[2];         /* +0x46                                         */
    uint8_t   data[];          /* +0x48 inline ring storage                     */
} spi_channel_t;

/*
 * ring_buf_t — the index/pointer ring carrying a CAN-TP session's bytes between
 * the CAN and SPI sides (ring_buf_* 0x5090..0x5360, ring_buf_write_notify
 * 0x59d8). Only the touched fields are named.
 */
typedef struct ring_buf {
    uint32_t  read_ptr;        /* +0x00 read cursor / head (saved on truncate)  */
    uint32_t  write_ptr;       /* +0x04 write cursor                            */
    uint32_t  capacity;        /* +0x08 ring capacity                           */
    uint32_t  notify_thresh;   /* +0x0c bytes_used threshold to notify consumer */
    void     *session;         /* +0x10 CAN session handle (cleared after cb)   */
    uint8_t   _pad14[4];       /* +0x14                                         */
    uint8_t  *data_buf;        /* +0x18 backing storage                         */
    uint8_t   flags;           /* +0x1c bit0 = 4-byte length-prefix mode        */
    uint8_t   _pad1d[0x23];    /* +0x1d                                         */
    uint32_t  pending_advance; /* +0x40 staged write-ahead amount               */
} ring_buf_t;

/*
 * can_session_t — the 0x28-byte heap CAN-TP session object (can_session_open
 * 0x35d4). The caller also keeps a 3-word session_handle: [0]=this, [1]=cb,
 * [2]=state.
 */
typedef struct can_session {
    uint32_t  seq;             /* +0x00 sequence counter                        */
    uint8_t   _pad04[0x10];    /* +0x04                                         */
    uint32_t  flags2;          /* +0x14 state/flags, zeroed on open             */
    uint32_t  timeout_ticks;   /* +0x18 converted from ms                       */
    void    **back_ptr;        /* +0x1c -> caller session_handle                */
    uint32_t  timer_cfg;       /* +0x20 = 0x5cfb (DAT_36ac)                     */
    uint8_t   flags;           /* +0x24 bit2 = unidirectional                   */
} can_session_t;

/* the session object seen by rx/tx-complete + table_advance (different view) */
typedef struct can_session_node {
    uint32_t  word0;           /* +0x00                                         */
    uint32_t  list_node;       /* +0x04 FreeRTOS list item                      */
    uint8_t   _pad08[0x10];    /* +0x08                                         */
    uint32_t  alt_list_node;   /* +0x18 overflow-mode list item                 */
    uint8_t   _pad1c[0x0c];    /* +0x1c                                         */
    uint32_t  error_flag;      /* +0x28 asserted 0 on complete                  */
    uint32_t  count;           /* +0x2c priority / table index                  */
    uint8_t   _pad30[0x38];    /* +0x30                                         */
    uint8_t   state;           /* +0x68 0=idle 1=active 2=done                  */
} can_session_node_t;

/*
 * spi_session_t — the per-fragment SPI session descriptor consumed by the SPI
 * frame builder (spi_tx_frame_send 0x55f0 / spi_frame_write_chunked 0x526a).
 */
typedef struct spi_session {
    uint8_t  *data_ptr;        /* +0x00 payload base (chunked)                  */
    uint8_t   _pad04[4];       /* +0x04                                         */
    uint32_t  total_bytes;     /* +0x08 total payload size (chunked)            */
    uint32_t  session_id;      /* +0x0c 24-bit session id -> frame[0..2]        */
    uint8_t   mode;            /* +0x10 0=single 1=frag+flag 2=frag             */
    uint8_t   _pad11[0x0f];    /* +0x11                                         */
    uint8_t  *payload;         /* +0x20 single-shot payload                     */
    uint32_t  payload_len;     /* +0x24 single-shot length -> frame[4]          */
} spi_session_t;

/* the SPI device object: ring at +0x594, single-shot send fn at +0x5a4 */
typedef struct spi_obj {
    void     *queue;           /* +0x00 SPI queue handle                        */
    uint8_t   _pad04[0x590];   /* +0x04                                         */
    void     *ring;            /* +0x594 channel object ('self' for chunked)    */
    uint8_t   _pad598[0x0c];   /* +0x598                                        */
    void    (*send_fn)(void *ring, void *frame); /* +0x5a4 single-shot dispatch */
} spi_obj_t;

/*
 * can_ctx_t — the CAN-FD controller context (can_fd_transmit 0x23a4). [0] is the
 * M_CAN MMIO base; the M_CAN registers below are at base + these offsets.
 */
typedef struct can_ctx {
    volatile uint32_t *mcan_base;   /* [0] M_CAN MMIO base                      */
    uint32_t  _w1;                  /* [1]                                      */
    void     *data_handle;          /* [2] data-phase MCAN handle               */
    void     *nom_handle;           /* [3] nominal-phase MCAN handle            */
    uint8_t   _pad10[4];            /* [4]                                      */
    uint8_t   nom_ready;            /* [5] byte                                 */
    uint8_t   _pad15[3];
    uint32_t  _w6;                  /* [6]                                      */
    uint32_t  tx_in_progress;       /* [7]                                      */
    uint32_t  prescaler;            /* [8]                                      */
} can_ctx_t;
/* M_CAN register offsets from mcan_base (Bosch M_CAN + NXP layout) */
#define MCAN_CCCR_OFF      0xe00u
#define MCAN_CCCR_EXT_OFF  0xe04u
#define MCAN_DATA_PRESC    0xe22u   /* data prescaler high-word (u16) */
#define MCAN_TXFIFO_OFF    0xe30u
#define MCAN_CCCR_INIT_CCE 0x30000u
#define MCAN_CCCR_FDOE     0x2000u
#define MCAN_CCCR_BRSE     0x1000u

/* SPI wire frame: 3-byte session id, flags+seq, length, <=8 payload (13 bytes) */
#define SPI_FRAME_LEN      13
#define SPI_FRAME_FLAG     0x10     /* frame[3] bit 4 */
#define SPI_CHUNK_MAX      8

/* spi_queue_send commands (cmd<6 = task path, cmd>=6 = ISR path) */
#define SPI_CMD_OPEN       1
#define SPI_CMD_NOTIFY     2
#define SPI_CMD_CLOSE      5
#define SPI_CMD_OPEN_ISR   6
#define SPI_CMD_NOTIFY_ISR 7

/* -------------------------------------------------- vendor callees (deferred) */
/* FreeRTOS kernel + Cortex-M port */
extern void   vTaskEnterCritical(void);                                  /* vendor freertos // 0x00000de8 */
extern void   vTaskExitCritical(void);                                   /* vendor freertos // 0x00000e04 (also FromISR notify) */
extern uint32_t prvIncrementSuspendedCounter(void);                      /* vendor freertos // 0x00000ce8 */
extern int    xTaskGetSchedulerState2(void);                             /* vendor freertos // 0x00000d1c (running == 2) */
extern void   vListInitialise(void *list);                               /* vendor freertos // 0x00005034 */
extern void   vListInsertEnd(void *list, void *item);                    /* vendor freertos // 0x0000504a */
extern void   vListInsert(void *list, uint32_t key, uint32_t tick);      /* vendor freertos // 0x00005062 */
extern void  *uxListRemove(void *item);                                  /* vendor freertos // 0x00005156 */
extern int    xQueueGenericSendFromISR(void *q);                         /* vendor freertos // 0x000012dc */
extern int    prvCopyDataToQueue(void *q);                               /* vendor freertos // 0x00001268 */
extern void   xTimerGenericCommand(void *t);                             /* vendor freertos // 0x00001090 */
extern void   prvFreeBlock(void *p);                                     /* vendor freertos // 0x00000d78 */
extern int    prvWriteBytesToBuffer(void *a, void *b);                   /* vendor freertos // 0x00000e2c */
extern int    prvWriteMessageToBuffer(uint32_t ticks, int wait);         /* vendor freertos // 0x00001424 */
extern void   port_yield_pend_sv(void);                                  /* vendor freertos // 0x000050e4 */
extern uint32_t port_set_interrupt_mask(void);                           /* vendor freertos // 0x000050a2 (also configASSERT trap) */
extern void   port_clear_interrupt_mask(uint32_t m);                     /* vendor freertos // 0x000050d6 */
extern void   spi_transfer_complete_handler(int cookie);                 /* vendor freertos // 0x000014b4 */
extern void   spi_notify_and_kick(void *obj, int blocking);              /* vendor freertos // 0x00001494 */
extern void   vPortStartFirstTask(void);                                 /* vendor freertos // 0x000002d0 */

/* NXP LPC55 SDK/HAL */
extern void   *heap_malloc(size_t size);                                 /* app    // 0x00001f28 (VanMoof allocator) */
extern uint32_t prvGetClockIndexByBase(void *base);                      /* vendor hal // 0x00000c28 */
extern void   clock_enable_peripheral(int id);                           /* vendor hal // 0x00004d94 */
extern int    MCAN_CalculateBitTimingParam(void *cfg, void *out);        /* vendor hal // 0x000005b0 */
extern void   can_fd_hw_init(void *drv);                                 /* vendor hal // 0x00002778 */
extern int    can_configure_tx_element(void *handle, void *tx_elem);     /* app    // 0x00000738 */
extern void   mcan_irq_enable(void *handle);                             /* vendor hal // 0x00004fa0 */
extern void   mcan_config_or_bits(void *r, uint32_t bits);               /* vendor hal // 0x0000501c */
extern void   mcan_filter_set_bits(void *r, uint32_t bits);              /* vendor hal // 0x0000520c */
extern uint32_t CLOCK_GetXtalOscFreq(void);                              /* vendor hal // 0x0000048c */
extern int    flash_read_sector(int sector);                             /* vendor hal // 0x0000205c */
extern int    can_mb_group_isr(void *mb, int timeout);                   /* app    // 0x000015b8 */
extern void   spi_slave_flexcomm_init(void);                             /* vendor hal // 0x00000f70 */

/* libc */
extern void  *mem_cpy(void *d, const void *s, size_t n);                 /* vendor libgcc // 0x00005d96 (memcpy) */
extern void  *mem_set(void *d, int c, size_t n);                         /* vendor libgcc // 0x00005db0 (memset) */

/* ----------------------------------------------------------- app prototypes */
/* SPI framing (spi_frame.c) */
int   spi_tx_frame_send(spi_obj_t *spi_obj, spi_session_t *sess);                 /* 0x55f0 */
void  spi_frame_write_chunked(void *ring, spi_session_t *sess, uint8_t *frame, uint32_t resume); /* 0x526a */
void  spi_tx_enqueue(spi_channel_t *ch);                                          /* 0x584a */
spi_channel_t *spi_channel_alloc(uint32_t capacity, uint32_t elem_size);          /* 0x5cc0 */
spi_channel_t *spi_channel_create(void);                                          /* 0x5d30 */
uint32_t ring_buf_bytes_used(ring_buf_t *rb);                                     /* 0x5090 */
uint32_t ring_buf_bytes_free(ring_buf_t *rb);                                     /* 0x50b8 */
void  ring_buf_advance_write_ptr(ring_buf_t *rb, void *dst);                       /* 0x52c6 */
uint32_t ring_buf_read(ring_buf_t *rb, void *dst, uint32_t want, uint32_t avail);  /* 0x52f2 */
uint32_t ring_buf_write(ring_buf_t *rb, const void *src, uint32_t count);          /* 0x5360 */
uint32_t ring_buf_write_capped(ring_buf_t *rb, const void *src, uint32_t want, uint32_t free_bytes, uint32_t min_required); /* 0x56dc */
bool  ring_buf_write_notify(ring_buf_t *rb, const void *src, uint32_t count, int *pend_sv); /* 0x59d8 */
int   spi_session_send(void *session, uint32_t hw0, uint32_t hw1, void *data, int length); /* 0x5ad4 */
int   spi_session_read(ring_buf_t *rbuf, void *dest, uint32_t max_len, int timeout_ms);    /* 0x5b8e */
int   spi_session_rx_frame(uint32_t session_ctx, uint32_t *can_msg, uint32_t flags);       /* 0x5964 */
void  spi_notify_host(spi_obj_t *spi_obj, uint32_t a, uint32_t b, int c);          /* 0x30b8 */
uint32_t spi_queue_send(void *obj, int cmd, uint32_t pa, void *pb, uint32_t pc, uint32_t a5, uint32_t a6); /* 0x2d6c */

/* CAN-TP sessions (can_session.c) */
bool  can_session_open(void **handle, int timeout_ms, int bidirectional, int callback); /* 0x35d4 */
int   can_session_write(ring_buf_t *session, uint8_t *buf, uint32_t len, int cookie);    /* 0x33ec */
void  can_session_rx_complete(can_session_node_t *s, uint32_t *wake_out);          /* 0x3244 */
void  can_session_tx_complete(can_session_node_t *s);                              /* 0x32d8 */
void  can_session_table_advance(void);                                             /* 0x3a6c */
void  can_session_timer_init(void);                                                /* 0x3580 */
uint32_t can_rx_frame_handler(void *rx_context, uint8_t *can_frame);               /* 0x37e4 */

/* CAN-FD transmit (can_tx.c) */
uint32_t can_fd_transmit(can_ctx_t *ctx, int classical_mode, uint32_t desc);       /* 0x23a4 */
void *can_tx_slot_alloc(void);                                                     /* 0x2338 */
void  can_tx_send_msg(uint32_t can_id, uint16_t cmd, int extra_count, ...);        /* 0x377c */

/* bridge tasks (bridge.c) */
void  spi_rx_send_loop(uint32_t *task_ctx);                                        /* 0x2938 (vm task) */
uint32_t spi_tx_send_loop(void *ring_buf, int blocking, int p3, uint32_t p4);      /* 0x2c5c (CanTX task) */
void  can_rx_dispatch_loop(uint32_t a, uint32_t b, int c, void *d);                /* 0x2eb8 (can task) */
int   can_tx_queue_send(void *q, void *out, int timeout_ms);                       /* 0x188c */
int   spi_rx_buf_advance(void *ch, int blocking);                                  /* 0x2ad0 */
int   spi_rx_buf_consume(void *ch, void *msg, int *woken);                          /* 0x577a */
void  func_0x4ed4(uint32_t *task_ctx);                                             /* 0x4ed4 (SPI frame submit) */

/* init + task-create (main.c) */
typedef void (*task_fn_t)(void *);   /* FreeRTOS-style task entry */
void  vm_can_init(void);                                                           /* 0x3ad8 (main) */
int   queue_msg_enqueue(task_fn_t task_fn, const char *name, int stack_words, void *arg, int priority, void **out_tcb); /* 0x214c */

#endif /* IMX8_BRIDGE_H */
