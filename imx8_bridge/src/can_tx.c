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
    uint8_t tx_elem[0x1c];
    uint8_t dlc_stride;

    if (ctx == NULL)
        return 1;
    base = ctx->mcan_base;                       /* [0] */

    MCAN_REG(base, 0x40c) |= 0x20u;              /* early, unconditional */

    if (ctx->tx_in_progress == 1)                /* [7] busy */
        return 2;

    prvGetClockIndexByBase((void *)base);

    MCAN_REG(base, MCAN_CCCR_OFF)     |= MCAN_CCCR_INIT_CCE;   /* +0xe00 INIT|CCE */
    MCAN_REG(base, MCAN_CCCR_EXT_OFF) |= 0x3u;                 /* +0xe04 */

    mem_set(tx_elem, 0, sizeof tx_elem);

    MCAN_REG(base, MCAN_CCCR_OFF) |= MCAN_CCCR_FDOE;          /* +0xe00 FDOE */
    ctx->tx_in_progress = 1;                                  /* [7] */
    ctx->prescaler      = 0x41;                               /* [8] */
    MCAN_REG(base, MCAN_CCCR_OFF) |= MCAN_CCCR_BRSE;          /* +0xe00 BRSE */

    /* Tx element descriptor: HW Tx FIFO addr, DLC stride (1 if DLC<8 else 2),
     * prescaler = 0x41/stride, FD/BRS/ESI flags. */
    dlc_stride = 1;                                           /* set per DLC<8 vs >=8 */
    *(void **)(tx_elem + 0x00) = (void *)((uintptr_t)base + MCAN_TXFIFO_OFF);
    *(uint16_t *)(tx_elem + 0x08) = dlc_stride;
    *(uint16_t *)(tx_elem + 0x0a) = (uint16_t)(0x41u / dlc_stride);
    *(void **)(tx_elem + 0x14) = (void *)(uintptr_t)desc;     /* payload ptr */

    if (can_configure_tx_element(ctx->nom_handle, tx_elem) != 0)  /* [3] nominal */
        return 2;

    if (classical_mode == 0) {
        /* CAN-FD: program the data-phase element + acceptance filters + BRS */
        uint8_t data_elem[0x1c];
        mem_set(data_elem, 0, sizeof data_elem);
        mcan_config_or_bits((void *)base, MCAN_CCCR_FDOE);
        mcan_filter_set_bits((void *)base, 0);
        MCAN_CalculateBitTimingParam(data_elem, data_elem);  /* BRS timing */
        *(void **)(data_elem + 0x14) = (void *)(uintptr_t)desc;
        if (can_configure_tx_element(ctx->data_handle, data_elem) != 0)  /* [2] data */
            return 2;
    }

    MCAN_REG(base, MCAN_DATA_PRESC) = ctx->prescaler;        /* +0xe22 high-word */
    mcan_irq_enable(classical_mode == 0 ? ctx->data_handle : ctx->nom_handle);
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

    uint8_t *slot = ctx + 8 + (size_t)count * 0x1c;    /* slot base */
    mem_set(slot, 0, 0x1c);
    *(uint32_t *)(slot + 0x20) = 0;                    /* status dword */
    ctx[0x5c] = (uint8_t)(count + 1);

    *(void **)(slot + 0x08) = g_can_tx_complete;       /* DAT_239c */
    *(void **)(slot + 0x0c) = g_can_tx_error;          /* DAT_23a0 */
    *(void **)(slot + 0x10) = *(void **)(g_can_ctx + 0x59c);  /* handle value */
    *(void **)(slot + 0x14) = g_can_ctx + 0x594;       /* ctx back-pointer */
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
