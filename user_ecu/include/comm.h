#ifndef USER_ECU_COMM_H
#define USER_ECU_COMM_H

#include <stdint.h>

/*
 * comm.h — unified multi-instance serial comm-port (CAN-FD / LPUART) driver.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * The "comm port" is a 4-instance unified serial IP (bases 0x40086000 /
 * 0x40087000 / 0x40088000 / 0x40089000, step 0x1000). Sibling instances run as
 * LPUART; instance 3 (0x40089000) is configured as CAN/CAN-FD via the bit-timing
 * block at base+0xc00. The MCU-side data path is a software descriptor ring +
 * eDMA (TX FIFO data at base+0xe30, RX at +0xe20), NOT hardware mailboxes; the
 * eDMA/DMAMUX engine is a separate peripheral at 0x40082000.
 *
 * This module holds the carved, self-contained pieces of that driver: instance
 * index mapping, the eDMA TCD builder + channel-IRQ enable, the LPUART line
 * config, the peripheral clock-mux select, the payload-queue receive/copy-out,
 * and teardown. The TX engine, IRQ dispatch and FIFO ISR body are follow-ups.
 */

/* eDMA Transfer Control Descriptor — the four 32-bit words edma_tcd_build writes. */
typedef struct edma_tcd {
    uint32_t word0;     /* +0x00 raw config word                       */
    uint32_t word1;     /* +0x04 addr_a + mult*((cfg>>12)&3)           */
    uint32_t word2;     /* +0x08 addr_b + mult*((cfg>>14)&3)           */
    uint32_t word3;     /* +0x0c trailing word, copied verbatim        */
} edma_tcd_t;

/* eDMA channel descriptor (partial) referenced via a comm-port handle. */
typedef struct edma_chan_desc {
    uint8_t  _pad00[0x08];  /* +0x00..+0x07                            */
    uint32_t edma_base;     /* +0x08 eDMA controller base (0x40082000) */
    uint8_t  channel;       /* +0x0c eDMA channel number               */
    uint8_t  _pad0d[3];     /* +0x0d..+0x0f                            */
} edma_chan_desc_t;

/*
 * Comm-port handle (partial reconstruction; only the OEM-touched fields are
 * named). The same handle is seen with its eDMA channel descriptor at +0x10
 * (TX-IRQ enable) and its register base/teardown fields at +0x160/+0x168.
 */
typedef struct commport_handle {
    void             *queue;          /* +0x000 payload/event queue handle      */
    void             *evt_list;       /* +0x004 blocked-task list head          */
    uint8_t           _pad08[0x10 - 0x08];
    edma_chan_desc_t *edma_chan;      /* +0x010 bound eDMA channel descriptor   */
    uint8_t           _pad14[0x160 - 0x14];
    volatile uint32_t *base;          /* +0x160 peripheral register base        */
    uint8_t           _pad164[4];     /* +0x164                                 */
    uint8_t           reset_pending;  /* +0x168 reset/enable flag               */
} commport_handle_t;

/*
 * Length-prefixed byte-ring payload queue (partial). queue_ring_copyout treats
 * read/size/storage as the ring read cursor / capacity / storage base.
 */
typedef struct commq {
    uint32_t read;          /* +0x00 ring read cursor (byte offset)            */
    uint32_t write;         /* +0x04 ring write cursor                         */
    uint32_t size;          /* +0x08 ring capacity (bytes)                     */
    uint8_t  _pad0c[0x10 - 0x0c];
    void    *rx_block;      /* +0x10 receiver-wait token (TCB)                 */
    void    *waiting_send;  /* +0x14 queued waiting-sender record              */
    uint8_t *storage;       /* +0x18 ring storage base                         */
    uint8_t  flags;         /* +0x1c bit0: records carry a 4-byte len prefix   */
} commq_t;

/* LPUART-variant line config descriptor (partial; byte offsets per the OEM). */
typedef struct commport_uart_cfg {
    uint8_t  data_bits;     /* +0x00 -> CTRL bit 7 (<<7)                       */
    uint8_t  enable;        /* +0x01 -> CTRL bit 0                             */
    uint8_t  f02;           /* +0x02 -> CTRL bit 5                             */
    uint8_t  f03;           /* +0x03 -> CTRL bit 4                             */
    uint8_t  f04;           /* +0x04 -> CTRL bit 3                             */
    uint8_t  _pad05[0x0c - 0x05];  /* +0x05..+0x0b (incl. the +0x08 baud word) */
    uint8_t  f0c;           /* +0x0c -> per-instance byte table [idx*2+0]      */
    uint8_t  f0d;           /* +0x0d -> per-instance byte table [idx*2+1]      */
    uint16_t f0e;           /* +0x0e -> CTRL bits [11:8]                       */
    uint8_t  f10;           /* +0x10 -> WATER bits [11:8]                      */
    uint8_t  f11;           /* +0x11 -> WATER bits [19:16]                     */
    uint8_t  f12;           /* +0x12 -> FORMAT bits [3:0]                      */
    uint8_t  f13;           /* +0x13 -> FORMAT bits [7:4]                      */
    uint8_t  f14;           /* +0x14 -> FORMAT bits [11:8]                     */
    uint8_t  f15;           /* +0x15 -> FORMAT bits [15:12]                    */
    /* target baud is read as a uint32 at +0x08 (overlaps _pad05). */
} commport_uart_cfg_t;

/* base -> instance index 0..3 (family 0x40086000 step 0x1000). // 0x00002c20 */
uint32_t commport_base_to_index(uint32_t base);

/* linear-search the 8-slot comm-port key registry; index 0..7 or 8. // 0x000024f4 */
int commport_registry_index(uint32_t key);

/* build an eDMA TCD from a packed config word. // 0x00008a7a */
void edma_tcd_build(edma_tcd_t *tcd, uint32_t cfg, uint32_t addr_a,
                    uint32_t addr_b, uint32_t word3);

/* set the eDMA channel-interrupt-enable bit for a comm-port handle. // 0x00008c46 */
void edma_chan_irq_enable(commport_handle_t *handle);

/* LPUART line/baud/FIFO config of a comm-port instance. 0 / 4 / 0x15e3. // 0x00002754 */
uint32_t commport_uart_config(uint32_t comm_base, const commport_uart_cfg_t *cfg,
                              uint32_t src_clk_hz);

/* gate + select a peripheral's functional clock source (PCC +0xff8). // 0x000022b0 */
int peripheral_clock_mux_select(uint32_t periph_base, uint32_t source);

/* length-prefixed payload-queue receive (blocking optional). // 0x00007550 */
int commport_queue_receive(commq_t *q, void *dst, uint32_t block_ms);

/* ring copy-out of min(count,avail) bytes; advances read cursor. // 0x000091ec */
unsigned int queue_ring_copyout(commq_t *ring, void *dst,
                                unsigned int count, unsigned int avail);

/* disable/teardown the comm-port instance at base 0x4009d000. // 0x000079c0 */
void commport_teardown(commport_handle_t *handle);

/* register a comm-port instance's ISR state (handle/callback/arg + NVIC). // 0x000078f0 */
uint32_t commport_isr_install(uint32_t comm_base, void *handle_mem, void *arg);

#endif /* USER_ECU_COMM_H */
