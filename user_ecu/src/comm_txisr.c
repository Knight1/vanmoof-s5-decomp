/*
 * comm_txisr.c — comm-port (CAN-FD) TX engine + ISR / eDMA-ring data path.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * The software-descriptor-ring + eDMA + FIFO data path for the comm port (see
 * comm.{c,h}). Translated via an analyze→adversarial-verify workflow (each
 * function cross-checked against the machine code). Kept separate from comm.c
 * to keep that module readable.
 *
 * Functions:
 *   commport_fifo_isr           @ 0x00002510  (FIFO PIO service / completion)
 *   commport_irq_dispatch_inst3 @ 0x00002190  (vector IRQ trampoline, inst 3)
 *   commport_rx_complete_cb     @ 0x000096ce  (RX-complete ring callback)
 *   commport_ring_drain         @ 0x00002d34  (TX/RX descriptor-ring drain)
 *   commport_can_transmit       @ 0x00007624  (TX/enqueue engine)
 *
 * The FIFO data regs are at base+0xe20 (RX pop) / +0xe30 (TX push); FIFO status
 * at +0xe04/+0xe08, command +0xe14. The eDMA/DMAMUX engine is at 0x40082000.
 */

#include <stdint.h>

#include "comm.h"       /* commport_base_to_index, commport_registry_index, edma_tcd_build, edma_chan_irq_enable, edma_chan_desc_t */
#include "util.h"       /* vmem_set */
#include "pcc.h"        /* nvic_irq_enable, gpio_base_to_bank */
#include "hal.h"        /* irqn_to_gpio_index */

#define MMIO32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

/* --- runtime / off-image tables ----------------------------------------- */
extern const uint32_t      g_commport_irq_base_tbl[];   /* 0x0000a510 (rodata) */
extern void * const        g_commport_irq_handle_tbl[]; /* 0x200016a0 (SRAM)   */
typedef void (*commport_irq_cb_t)(uint32_t arg, void *handle);
extern const commport_irq_cb_t g_commport_irq_cb_tbl[]; /* 0x200016c4 (SRAM)   */
extern const uint32_t      g_edma_tcd_base[];           /* 0x0000a508 {bank0..2} */

/* SRAM per-instance idle/fill-pattern byte table (commport_fifo_isr). */
#define commport_fill_pattern   ((volatile uint8_t *)0x200070e0u)   /* @0x2750 */

/* eDMA-ring + TCD scratch bases (commport_ring_drain / commport_can_transmit). */
#define COMMPORT_TX_RING       0x20001564u  /* DAT_000078b8 / DAT_00002ed0 */
#define EDMA_CITER_MASK        0x03ff0000u  /* DAT_000078bc / DAT_00002ecc */
#define COMMPORT_TCD_SCRATCH   0x20000570u  /* DAT_000078c0 / DAT_00002ec8 */
#define CHAIN_TCD_DST          0x20001e60u  /* DAT_000078c8 */
#define CHAIN_TCD_ATTR         0x00010203u  /* DAT_000078cc */
#define CHAIN_TCD_SRC          0x20001e64u  /* DAT_000078d0 */

/* --- FreeRTOS port (vendor) + not-yet-translated VanMoof helpers --------- */
extern uint32_t vPortRaiseBASEPRI(void);                 /* 0x0000950a (returns prior BASEPRI) */
extern void     vPortSetBASEPRI(uint32_t basepri);       /* 0x00009520 */
extern uint32_t FUN_00006e20(void *ring, void *frame);   /* ring/descriptor push (reads r0 only) */
extern int      FUN_00006bfc(void *notify_obj);          /* task signal/notify -> woken? */

/* --- per-purpose partial state structs (distinct views of related objects) */

/* commport_fifo_isr transfer descriptor. */
typedef struct commport_xfer {
    uint8_t  *tx_src;       /* +0x00 */
    uint8_t  *rx_dst;       /* +0x04 */
    int32_t   tx_remaining; /* +0x08 */
    int32_t   rx_remaining; /* +0x0c */
    int8_t    inflight;     /* +0x10 words issued, not yet received */
    uint8_t   _p11[0x13];   /* +0x11..+0x23 */
    uint8_t   elem_width;   /* +0x24 (>7 => 16-bit) */
    uint8_t   clr_shift;    /* +0x25 */
    uint8_t   _p26[6];      /* +0x26..+0x2b */
    uint8_t   rx_wm;        /* +0x2c */
    uint8_t   tx_wm;        /* +0x2d */
    uint8_t   _p2e[2];      /* +0x2e..+0x2f */
} commport_xfer_t;

/* commport_rx_complete_cb ring/descriptor state. */
typedef struct commport_ring {
    uint8_t   _p00[0x24];
    void     *notify_obj;   /* +0x24 bound task-notify object */
    uint8_t   _p28[0x38 - 0x28];
    uint32_t  head;         /* +0x38 */
    uint32_t  tail;         /* +0x3c */
    uint32_t  f40;          /* +0x40 mode/flag */
    uint8_t   _p44;         /* +0x44 */
    int8_t    seq;          /* +0x45 signed sequence counter */
} commport_ring_t;

/* commport_can_transmit job/queue context (param_2). */
typedef struct commport_tx_job {
    uint32_t          state;     /* +0x00 SM state 0=idle 1=RX 2=TX */
    uint8_t           mode;      /* +0x04 transfer mode 1..4 (4 => 32-bit) */
    uint8_t           _p05[0x0b];
    edma_chan_desc_t *edma;      /* +0x10 eDMA channel descriptor */
    uint32_t          slot[8];   /* +0x14 interleaved {buf,len} ring */
    uint8_t           wr_idx;    /* +0x34 job ring write index (mod 4) */
} commport_tx_job_t;

/* ring drain takes the same object viewed as {state, fmt}. */
typedef struct commport_xfer_req {
    uint32_t state;             /* +0x00 ==1 => TX, else RX */
    uint8_t  fmt;               /* +0x04 word-size selector (== job->mode) */
} commport_xfer_req_t;

/* per-channel TX-ring state block (stride 0x38, base COMMPORT_TX_RING+chan*0x38). */
typedef struct tx_chan_state {
    uint8_t _p00[0x0d];
    uint8_t desc_idx;   /* +0x0d length-table index (mod 2) */
    uint8_t inflight;   /* +0x0e in-flight TCD count */
    uint8_t tcd_idx;    /* +0x0f TCD ping/pong flip */
    uint8_t rd_idx;     /* +0x10 descriptor read index (mod 4) */
    uint8_t _p11[0x23];
    uint8_t flip;       /* +0x34 src/dst flip flag */
} tx_chan_state_t;      /* 0x38 bytes */

/* packed {buf,len} descriptor cell (.buf @+4, .len @+8). */
typedef struct tx_desc_cell {
    uint32_t _r0;       /* +0x00 */
    uint32_t buf;       /* +0x04 */
    uint32_t len;       /* +0x08 */
} tx_desc_cell_t;

void commport_ring_drain(uint32_t base, commport_xfer_req_t *req);   /* fwd (called by transmit) */

/*
 * commport_fifo_isr — FIFO-status-driven PIO service for one transfer. // 0x00002510
 *
 * Pumps bytes/words between xfer->tx_src/rx_dst and the data FIFOs (push +0xe20,
 * pop +0xe30), throttled by FIFO fill/watermark (+0xe04/+0xe08); on completion
 * latches result=0x15e1 and tail-calls the completion callback, else re-arms the
 * watermark. (The `next_iter` reset belongs at the top of the inner loop — a
 * verify-corrected latent infinite loop.)
 */
void commport_fifo_isr(uint32_t base, commport_xfer_t *xfer)
{
    int     *w = (int *)xfer;
    uint8_t *b = (uint8_t *)xfer;

    if (xfer->tx_remaining == 0 && xfer->rx_remaining == 0 && xfer->inflight == 0) {
        MMIO32(base + 0xe14) = 0xc;                          /* advance/flush */
        MMIO32(base + 0xe08) =
            ((uint32_t)(b[0x2d] & 0xf) << 16) |
            ((uint32_t)(b[0x2c] & 0xf) <<  8) |
            (MMIO32(base + 0xe08) & 0xfff0ffffu);
        w[6] = 0x15e1;                                       /* result = done */
        if ((void *)w[7] == 0) {
            return;
        }
        ((void (*)(uint32_t, commport_xfer_t *, int, int))w[7])(base, xfer, w[6], w[8]);
        return;
    }

    int      idx       = commport_registry_index(base);
    uint32_t ctrl_e00  = MMIO32(base + 0xe00);
    uint32_t flags     = (uint32_t)w[10];                    /* ctrl_flags @0x28 */
    uint32_t push_tmpl =
        (~(1u << ((b[0x25] + 0x10) & 0xff)) & 0xf0000u) |
        (flags & 0x200000u) |
        ((uint32_t)(b[0x24] & 0xf) << 24);
    uint32_t half_wm   = (ctrl_e00 >> 1) & 0x18u;
    uint32_t flags_eot = flags & 0x100000u;

    int next_iter;
    do {
        for (;;) {
            next_iter = 0;                                   /* reset each inner pass */

            if ((MMIO32(base + 0xe04) & 0x40) != 0) {        /* RX word waiting */
                uint32_t rxw = MMIO32(base + 0xe30);
                if (xfer->rx_remaining != 0) {
                    *xfer->rx_dst++ = (uint8_t)rxw;
                    xfer->rx_remaining--;
                    if (b[0x24] > 7) {                       /* 16-bit element */
                        *xfer->rx_dst++ = (uint8_t)(rxw >> 8);
                        xfer->rx_remaining--;
                    }
                }
                next_iter = 1;
                xfer->inflight = (int8_t)(xfer->inflight - 1);
            }

            int      inflight = (int8_t)b[0x10];
            uint32_t pending  = (inflight > 0) ? (uint8_t)b[0x10] : 0u;

            if ((MMIO32(base + 0xe04) & 0x20) == 0 || half_wm <= pending) {  /* no TX room */
                break;
            }

            uint8_t  ew = b[0x24];
            uint32_t word;

            if (xfer->tx_remaining == 0) {
                if ((uint32_t)xfer->rx_remaining < ((pending + 1) << (ew >> 3))) {
                    break;
                }
                word = ((uint32_t)commport_fill_pattern[idx]) |
                       ((uint32_t)commport_fill_pattern[idx] << 8);
                if ((uint32_t)xfer->rx_remaining == ((pending + 1) << (ew >> 3))) {
                    push_tmpl |= flags_eot;
                }
            } else if (*(uint32_t *)&w[0] == 0) {            /* tx_src == NULL: filler */
                word = ((uint32_t)commport_fill_pattern[idx]) |
                       ((uint32_t)commport_fill_pattern[idx] << 8);
                if ((uint32_t)xfer->rx_remaining == ((pending + 1) << (ew >> 3))) {
                    push_tmpl |= flags_eot;
                }
            } else {
                uint8_t lo = *xfer->tx_src++;
                word = lo;
                xfer->tx_remaining--;
                if (ew > 7) {
                    word = (uint32_t)lo | ((uint32_t)(*xfer->tx_src++) << 8);
                    xfer->tx_remaining--;
                }
                if (xfer->tx_remaining == 0) {
                    push_tmpl |= flags_eot;
                }
            }

            MMIO32(base + 0xe20) = word | push_tmpl;         /* push word */
            xfer->inflight = (int8_t)(xfer->inflight + 1);
        }
    } while (next_iter);

    if (xfer->tx_remaining == 0 && xfer->rx_remaining == 0 && xfer->inflight == 0) {
        MMIO32(base + 0xe08) &= 0xfffff0ffu;
        MMIO32(base + 0xe10) |= 4u;                          /* flush/disable */
        return;
    }

    uint32_t rx_words  = (uint32_t)xfer->rx_remaining >> (b[0x24] >> 3);
    int      inflight2 = (int8_t)b[0x10];
    uint32_t pending2  = (inflight2 > 0) ? (uint8_t)b[0x10] : 0u;

    if (xfer->tx_remaining == 0) {
        if (pending2 < rx_words) {
            goto rearm_rx;
        }
        MMIO32(base + 0xe14) = 4;                            /* step */
    }

    if (rx_words == 0) {
        if (xfer->tx_remaining != 0) {
            return;
        }
        if (pending2 == 0) {
            return;
        }
        if (((MMIO32(base + 0xe08) & 0xfffffu) >> 16) + 1 <= pending2) {
            return;
        }
        MMIO32(base + 0xe08) = (MMIO32(base + 0xe08) & 0xfff0ffffu) | ((pending2 - 1) << 16);
        return;
    }

rearm_rx:
    if (rx_words < ((MMIO32(base + 0xe08) & 0xfffffu) >> 16) + 1) {
        MMIO32(base + 0xe08) = (MMIO32(base + 0xe08) & 0xfff0ffffu) | ((rx_words - 1) << 16);
    }
}

/*
 * commport_irq_dispatch_inst3 — vector IRQ trampoline for comm-port inst 3. // 0x00002190
 *
 * idx = irqn_to_gpio_index(0x40089000); then tail-calls the registered handler
 *   g_commport_irq_cb_tbl[idx](g_commport_irq_base_tbl[idx], g_commport_irq_handle_tbl[idx]).
 * One of a family of byte-identical trampolines differing only in the base.
 */
void commport_irq_dispatch_inst3(void)
{
    int idx = irqn_to_gpio_index(0x40089000);
    g_commport_irq_cb_tbl[idx](g_commport_irq_base_tbl[idx], g_commport_irq_handle_tbl[idx]);
}

/*
 * commport_rx_complete_cb — RX-complete callback for the SW ring. // 0x000096ce
 *
 * Under a BASEPRI critical section: if the ring is non-empty (head < tail),
 * push the received frame (FUN_00006e20) and maintain a signed seq counter at
 * +0x45 — at the -1 sentinel it fires the bound task-notify (FUN_00006bfc) and
 * reports a higher-priority wake via *signal_out; else increments (trap at 0x7f).
 * Returns 1 if a frame was consumed, 0 if empty.
 */
int commport_rx_complete_cb(commport_ring_t *ring, void *frame, int *signal_out)
{
    if (ring == 0) {
        vPortRaiseBASEPRI();
        for (;;) { }
    }
    if (frame == 0 && ring->f40 != 0) {
        vPortRaiseBASEPRI();
        for (;;) { }
    }

    uint32_t saved = vPortRaiseBASEPRI();
    int result;

    if (ring->head < ring->tail) {
        int8_t seq = ring->seq;
        FUN_00006e20(ring, frame);
        if (seq == -1) {
            if (ring->notify_obj != 0 &&
                FUN_00006bfc(&ring->notify_obj) != 0 &&
                signal_out != 0) {
                *signal_out = 1;
            }
        } else {
            if (seq == 0x7f) {
                vPortRaiseBASEPRI();
                for (;;) { }
            }
            ring->seq = (int8_t)(seq + 1);
        }
        result = 1;
    } else {
        result = 0;
    }

    vPortSetBASEPRI(saved);
    return result;
}

/*
 * commport_ring_drain — TX/RX software-descriptor-ring drain. // 0x00002d34
 *
 * For each pending descriptor of the channel, clamp the per-burst length to the
 * FIFO depth, rebuild an eDMA TCD in a per-channel ping/pong scratch slot
 * (edma_tcd_build), toggle the eDMA-group flip flag, advance the cursor, and on
 * a fully-drained descriptor advance the slot index — until 2 TCDs are in flight
 * or the ring is empty. (state==1 => TX with attr bit12; else RX with bit14.)
 */
void commport_ring_drain(uint32_t base, commport_xfer_req_t *req)
{
    const int idx = commport_base_to_index(base);
    volatile uint8_t *const ring =
        (volatile uint8_t *)(COMMPORT_TX_RING + (uint32_t)idx * 0x38u);

    for (;;) {
        for (;;) {
            if (ring[0xe] > 1u) {                            /* 2 TCDs in flight */
                return;
            }

            const uint8_t slot = ring[0x10];
            const uint8_t flip = ring[0x34];
            volatile uint32_t *desc = (volatile uint32_t *)
                (COMMPORT_TX_RING + ((uint32_t)idx * 7u + slot + 2u) * 8u);

            if (desc[2] == 0u) {                             /* record+8 = remaining */
                return;
            }

            const uint32_t state  = req->state;
            const uint32_t dir_tx = (state == 1u);
            const uint32_t dir_rx = !dir_tx;

            uint32_t arg3, arg4;
            if (dir_tx) {
                arg3 = desc[1];                              /* descriptor addr */
                arg4 = base + 0xe20u;
            } else {
                arg3 = base + 0xe30u;
                arg4 = desc[1];
            }

            uint32_t *tcd_cur = (uint32_t *)
                (COMMPORT_TCD_SCRATCH + ((uint32_t)ring[0xf] + (uint32_t)idx * 2u) * 0x10u);
            ring[0xf] = (uint8_t)((ring[0xf] + 1u) & 1u);
            uint32_t *tcd_nxt = (uint32_t *)
                (COMMPORT_TCD_SCRATCH + ((uint32_t)ring[0xf] + (uint32_t)idx * 2u) * 0x10u);

            const uint32_t rem = desc[2];
            uint32_t len;
            if (rem >= 0x2000u) {
                len = 0x1000u;
            } else if (rem <= 0x1000u) {
                len = (uint16_t)rem;
            } else {
                uint32_t t = (rem >> 1) & 0xffffu;
                if ((rem & 6u) != 0u) {
                    t = (uint16_t)(t & ~3u);
                }
                len = t;
            }

            *(volatile uint16_t *)
                (COMMPORT_TX_RING + ((uint32_t)idx * 0x1cu + (uint32_t)ring[0xd] + 4u) * 2u)
                    = (uint16_t)len;
            ring[0xd] = (uint8_t)((ring[0xd] + 1u) & 1u);

            const uint32_t fmt = req->fmt;
            uint32_t xfer_field;
            if (fmt == 4u) {
                xfer_field = 0x200u;
            } else {
                xfer_field = ((fmt - 1u) << 8) & 0x300u;
            }
            const uint32_t loops = len / fmt;

            uint32_t attr =
                  ((uint32_t)flip << 5)
                | xfer_field
                | (((uint32_t)flip ^ 1u) << 4)
                | (dir_tx << 0xc)
                | (dir_rx << 0xe)
                | (EDMA_CITER_MASK & ((loops - 1u) << 0x10))
                | 3u;

            edma_tcd_build((edma_tcd_t *)tcd_cur, attr, arg3, arg4, (uint32_t)(uintptr_t)tcd_nxt);

            ring[0xe] = (uint8_t)(ring[0xe] + 1u);
            ring[0x34] ^= 1u;

            desc = (volatile uint32_t *)
                (COMMPORT_TX_RING + ((uint32_t)idx * 7u + slot + 2u) * 8u);
            desc[2] -= len;
            desc[1] += len;

            if (desc[2] == 0u) {
                desc[1] = 0u;
                ring[0x10] = (uint8_t)((ring[0x10] + 1u) & 3u);
                break;                                       /* -> outer loop */
            }
        }
    }
}

/*
 * commport_can_transmit — TX/enqueue engine. // 0x00007624
 *
 * Validates + enqueues {buf,len} into the global+job descriptor rings; if the
 * channel SM is idle, primes the hardware (builds the primary + two chain eDMA
 * TCDs, sets the CAN FIFO direction at +0xe00, arms the eDMA channel and kicks
 * TX at +0xc00). Then drains the ring (commport_ring_drain) for both the
 * just-primed and the already-active paths; on an active SM, enables the eDMA
 * error IRQ. Reached via the device-manager ops vtable (no direct C callers).
 */
void commport_can_transmit(uint32_t base, commport_tx_job_t *job, uint32_t buf, uint32_t len)
{
    int      chan;
    uint8_t *ring = (uint8_t *)(uintptr_t)COMMPORT_TX_RING;

    edma_chan_irq_enable((commport_handle_t *)job);          /* +0x50 group bit */
    chan = commport_base_to_index(base);

    if (job == 0 || len == 0 || ((buf | len) & 3) != 0) {
        goto epilogue;
    }
    {
        unsigned w = job->wr_idx;
        if (*(uint32_t *)((uint8_t *)job + (w + 2) * 8 + 8) != 0) {  /* slot occupied */
            goto epilogue;
        }
    }

    {
        unsigned w  = job->wr_idx;
        unsigned c7 = (unsigned)chan * 7u;

        *(uint32_t *)(ring + (w + c7 + 2) * 8 + 4) = buf;
        *(uint32_t *)(ring + (w + c7 + 2) * 8 + 8) = len;
        *(uint32_t *)((uint8_t *)job + (w + 2) * 8 + 4) = buf;
        *(uint32_t *)((uint8_t *)job + (w + 2) * 8 + 8) = len;

        job->wr_idx = (uint8_t)((job->wr_idx + 1) & 3);
    }

    if (job->state == 0) {                                   /* SM idle: prime HW */
        tx_chan_state_t *st = (tx_chan_state_t *)(ring + (unsigned)chan * 0x38u);
        unsigned c7 = (unsigned)chan * 7u;
        unsigned rd, words, mode, attr;
        unsigned saddr;
        edma_chan_desc_t *rec;
        uint32_t main_tcd, chain_tcd;
        int bank;
        tx_desc_cell_t *cell;

        job->state = 2;

        rd   = st->rd_idx;
        cell = (tx_desc_cell_t *)(ring + (rd + c7 + 2) * 8);

        words = cell->len;
        if (words >= 0x2000u) {
            words = 0x1000u;
        } else if (words <= 0x1000u) {
            words = (uint16_t)cell->len;
        } else {
            unsigned v = cell->len;
            if ((v & 6u) == 0) {
                words = (v & 0x1ffffu) >> 1;
            } else {
                words = (uint16_t)((uint16_t)((v << 15) >> 16) & 0xfffcu);
            }
        }

        saddr        = cell->buf;
        st->inflight = 1;
        st->flip     = 0;

        mode = job->mode;
        rec  = job->edma;
        if (mode == 4) {
            attr = 0x223u;
        } else {
            attr = ((mode - 1) << 8) & 0x300u;
            attr |= 0x23u;
        }
        attr = (EDMA_CITER_MASK & (((words / mode) - 1) << 16)) | attr | 0x4000u;

        main_tcd = COMMPORT_TCD_SCRATCH + (unsigned)chan * 0x20u;

        bank = gpio_base_to_bank(rec->edma_base);
        edma_tcd_build((edma_tcd_t *)(uintptr_t)
                           (g_edma_tcd_base[bank] + (unsigned)rec->channel * 0x10u),
                       attr, base + 0xe30u, saddr, main_tcd);

        MMIO32(rec->edma_base + (unsigned)rec->channel * 0x10u + 0x408u) = attr;

        st = (tx_chan_state_t *)(ring + (unsigned)chan * 0x38u);
        *(int16_t *)(ring + ((unsigned)chan * 0x1c + st->desc_idx + 4) * 2) = (int16_t)words;
        st->desc_idx = (uint8_t)((st->desc_idx + 1) & 1);

        cell = (tx_desc_cell_t *)(ring + (rd + c7 + 2) * 8);
        cell->len -= words;
        cell->buf += words;
        if (cell->len == 0) {
            cell->buf  = 0;
            st->rd_idx = (uint8_t)((st->rd_idx + 1) & 3);
        }

        chain_tcd = COMMPORT_TCD_SCRATCH + ((unsigned)chan * 2u + 1u) * 0x10u;
        {
            uint32_t a2, a3;
            if (job->state == 1) { a2 = CHAIN_TCD_SRC; a3 = base + 0xe20u; }
            else                 { a2 = base + 0xe30u; a3 = CHAIN_TCD_DST; }
            edma_tcd_build((edma_tcd_t *)(uintptr_t)main_tcd, CHAIN_TCD_ATTR, a2, a3, chain_tcd);

            if (job->state == 1) { a2 = CHAIN_TCD_SRC; a3 = base + 0xe20u; }
            else                 { a2 = base + 0xe30u; a3 = CHAIN_TCD_DST; }
            edma_tcd_build((edma_tcd_t *)(uintptr_t)chain_tcd, CHAIN_TCD_ATTR, a2, a3, main_tcd);
        }

        rec = job->edma;
        if (job->state == 1) {
            MMIO32(base + 0xe00u) |= 0x1000u;                /* RX dir */
        } else {
            MMIO32(base + 0xe00u) |= 0x2000u;                /* TX dir */
        }

        {
            uint8_t  ch   = rec->channel;
            uint32_t edma = rec->edma_base;
            unsigned grp  = (unsigned)(ch >> 5);

            MMIO32(edma + (unsigned)ch * 0x10u + 0x400u) |= 1u;
            MMIO32(edma + 0x20u + grp * 4u) |= 1u << (ch & 0x1f);

            if (((int)(MMIO32(edma + ((unsigned)ch + 0x40u) * 0x10u) << 0x1e)) >= 0) {
                MMIO32(edma + (unsigned)ch * 0x10u + 0x408u) |= 4u;
            }

            MMIO32(base + 0xc00u) |= 1u;                     /* TX kick */
        }
    }

    commport_ring_drain(base, (commport_xfer_req_t *)job);   /* both paths */

epilogue:
    if (job->state != 0) {                                   /* tail-call body inlined */
        edma_chan_desc_t *rec = job->edma;
        uint8_t  ch  = rec->channel;
        unsigned grp = (unsigned)(ch >> 5);
        MMIO32(rec->edma_base + 0x48u + grp * 4u) |= 1u << (ch & 0x1f);
    }
}
