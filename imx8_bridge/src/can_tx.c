/*
 * can_tx.c — VanMoof S5 imx8_bridge CAN-FD transmit path.
 *
 * Builds and submits CAN-FD (and classical CAN) Tx elements to the NXP/Bosch
 * M_CAN controller, allocates pending-TX descriptor slots, and emits the
 * variadic diagnostic/control message frames. Translated from the OEM image
 * (NXP LPC55S69, base 0x0). The M_CAN HAL helpers (bit-timing, filter, IRQ
 * enable) and the SDK clock-index lookup are vendor; the MMIO writes are verbatim.
 *
 * OEM: can_fd_transmit 0x23a4, can_tx_slot_alloc 0x2338, can_tx_send_msg 0x377c.
 */

#include "imx8_bridge.h"
#include <stdarg.h>

/* Global CAN state + descriptor tables reached via literal pools. */
extern uint8_t *g_can_ctx;          /* DAT_2398 — global CAN context block */
extern void    *g_can_tx_complete;  /* DAT_239c — tx-complete callback ptr */
extern void    *g_can_tx_error;     /* DAT_23a0 — tx-error callback ptr */
extern void   **g_can_msg_root;     /* DAT_37dc — ptr-to-ptr CAN context root */
extern void    *g_can_msg_chan;     /* DAT_37e0 — SPI channel selector */

/* Per-clock-index bit-rate tables (vendor SDK data, indexed by clock index). */
extern const uint8_t can_dlc_table[];      /* DAT_2768 — nominal DLC bytes (stride 2) */
extern const uint8_t can_fd_dlc_table[];   /* DAT_2770 — FD data DLC bytes (stride 1) */
extern const uint8_t can_bittiming_cfg[];  /* DAT_276c — bit-timing structs (stride 0x10) */

/* M_CAN byte-offset MMIO helper (base is the controller's MMIO base). */
#define MCAN_REG(base, off) MMIO32((uintptr_t)(base) + (off))

/* ------------------------------------------------------------------- 0x23a4 */

/* Submit a CAN-FD (classical_mode==0) or classical (classical_mode!=0) Tx
 * descriptor. Returns 0=ok, 1=null ctx, 2=busy/error. The register order below
 * follows the verified disassembly: the +0x40c bit is set unconditionally before
 * the busy check; the tx-in-progress / prescaler fields are written only after
 * the CCCR config + memset. */
uint32_t can_fd_transmit(can_ctx_t *ctx, int classical_mode, uint32_t desc)
{
    volatile uint32_t *base;
    uint8_t  tx_elem[0x1c];
    uint32_t clk;
    uint8_t  dlc_stride;

    if (ctx == NULL)
        return 1;
    base = ctx->mcan_base;                       /* [0] */

    MCAN_REG(base, 0x40c) |= 0x20u;              /* early, unconditional */

    if (ctx->tx_in_progress == 1)                /* [7] busy */
        return 2;

    clk = prvGetClockIndexByBase((void *)base);  /* per-instance clock index */

    MCAN_REG(base, MCAN_CCCR_OFF)     |= MCAN_CCCR_INIT_CCE;   /* +0xe00 INIT|CCE */
    MCAN_REG(base, MCAN_CCCR_EXT_OFF) |= 0x3u;                 /* +0xe04 */

    mem_set(tx_elem, 0, sizeof tx_elem);

    MCAN_REG(base, MCAN_CCCR_OFF) |= MCAN_CCCR_FDOE;          /* +0xe00 FDOE */
    ctx->tx_in_progress = 1;                                  /* [7] */
    ctx->prescaler      = 0x41;                               /* [8] */
    MCAN_REG(base, MCAN_CCCR_OFF) |= MCAN_CCCR_BRSE;          /* +0xe00 BRSE */

    /* nominal Tx element: HW Tx FIFO addr, DLC stride from the per-clock table
     * (1 if DLC<8 else 2), prescaler = 0x41/stride, FD/BRS/ESI flags. */
    dlc_stride = (can_dlc_table[clk * 2] < 8) ? 1 : 2;
    *(void **)(tx_elem + 0x00) = (void *)((uintptr_t)base + MCAN_TXFIFO_OFF);
    *(uint16_t *)(tx_elem + 0x08) = dlc_stride;
    *(uint16_t *)(tx_elem + 0x0a) = (uint16_t)(0x41u / dlc_stride);
    *(void **)(tx_elem + 0x14) = (void *)(uintptr_t)desc;     /* payload ptr */

    if (can_configure_tx_element(ctx->nom_handle, tx_elem) != 0)  /* [3] nominal */
        return 2;
    ctx->nom_ready = 1;                                       /* [5] strb r7,[r6,#0x5] */

    if (classical_mode == 0) {
        /* CAN-FD: data-phase element + acceptance filters + BRS bit-timing */
        uint8_t data_elem[0x1c];
        uint8_t fd_stride = (can_fd_dlc_table[clk] < 8) ? 1 : 2;
        mem_set(data_elem, 0, sizeof data_elem);
        mcan_config_or_bits((void *)base, MCAN_CCCR_FDOE);
        mcan_filter_set_bits((void *)base, 0);
        MCAN_CalculateBitTimingParam((void *)&can_bittiming_cfg[clk * 0x10], data_elem);
        *(uint16_t *)(data_elem + 0x08) = fd_stride;
        *(void **)(data_elem + 0x14) = (void *)(uintptr_t)desc;
        if (can_configure_tx_element(ctx->data_handle, data_elem) != 0)  /* [2] data */
            return 2;
    } else {
        *(uint8_t *)((uint8_t *)ctx + 0x04) = 1;             /* classical: byte @ctx+0x4 */
    }

    /* 16-bit store of the bit-timing high word at base+0xe22 (NOT 32-bit). */
    *(volatile uint16_t *)((uintptr_t)base + MCAN_DATA_PRESC) = (uint16_t)ctx->prescaler;
    mcan_irq_enable(ctx->data_handle);                       /* always data_handle */
    return 0;
}

/* ------------------------------------------------------------------- 0x2338 */

/* Allocate one of up to 3 pending CAN-TX descriptor slots (0x1c each) from the
 * global CAN state block at g_can_ctx+0x5a8. Installs the tx-complete/tx-error
 * callbacks and the handle/back-pointer. Returns the slot body, or NULL if the
 * slot array is full or the array is busy. */
void *can_tx_slot_alloc(void)
{
    uint8_t *ctx = *(uint8_t **)(g_can_ctx + 0x5a8);   /* TX slot array header */
    uint8_t  count = ctx[0x5c];                        /* in-use slot count */

    if (count >= 3 || ctx[0x04] != 0)                  /* full or busy */
        return NULL;

    uint8_t *slot = ctx + 8 + (size_t)count * 0x1c;    /* slot body (returned) */
    mem_set(slot, 0, 0x1c);
    /* OEM stores all fields through the raw element base (slot-8), so the field
     * offsets relative to the returned slot body are 8 lower than the raw +0x8.. */
    *(uint32_t *)(slot + 0x18) = 0;                    /* status dword  (raw +0x20) */
    ctx[0x5c] = (uint8_t)(count + 1);

    *(void **)(slot + 0x00) = g_can_tx_complete;       /* DAT_239c     (raw +0x08) */
    *(void **)(slot + 0x04) = g_can_tx_error;          /* DAT_23a0     (raw +0x0c) */
    *(void **)(slot + 0x08) = *(void **)(g_can_ctx + 0x59c);  /* handle (raw +0x10) */
    *(void **)(slot + 0x0c) = g_can_ctx + 0x594;       /* ctx back-ptr (raw +0x14) */
    return slot;
}

/* ------------------------------------------------------------------- 0x377c */

/* Emit a diagnostic/control CAN message over the SPI-session layer: a 0x1e-byte
 * frame holding the CAN-ID, a u16 command, and up to `extra_count` payload words
 * (copied from frame+6). No-op until the global CAN context is installed. */
void can_tx_send_msg(uint32_t can_id, uint16_t cmd, int extra_count, ...)
{
    uint8_t frame[0x1e];
    void  **root;
    void   *session;
    int     i;
    va_list ap;

    if (*g_can_msg_root == NULL)               /* context not installed yet */
        return;

    mem_set(frame, 0, sizeof frame);
    *(uint32_t *)(frame + 0) = can_id;
    *(uint16_t *)(frame + 4) = cmd;

    va_start(ap, extra_count);
    for (i = 0; i < extra_count; i++)
        *(uint32_t *)(frame + 6 + i * 4) = va_arg(ap, uint32_t);  /* +6 (verifier) */
    va_end(ap);

    root    = (void **)(*g_can_msg_root);      /* *(*DAT_37dc) */
    session = *(void **)((uint8_t *)root[0] + 0x590);   /* (*ctx)[0] + 0x590 */
    spi_session_send(session, (uint32_t)(uintptr_t)g_can_msg_chan, 0, frame, 0x1e);
}
