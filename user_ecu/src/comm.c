/*
 * comm.c — unified multi-instance serial comm-port (CAN-FD / LPUART) driver.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M, VFPv4 hard-float, FreeRTOS). Image base 0x0.
 *
 * Functions (carved, self-contained pieces of the comm-port driver):
 *   commport_base_to_index      @ 0x00002c20  (base -> instance index 0..3)
 *   commport_registry_index     @ 0x000024f4  (8-slot key registry lookup)
 *   edma_tcd_build              @ 0x00008a7a  (eDMA Transfer Control Descriptor)
 *   edma_chan_irq_enable        @ 0x00008c46  (eDMA channel IRQ enable)
 *   commport_uart_config        @ 0x00002754  (LPUART line/baud/FIFO config)
 *   peripheral_clock_mux_select @ 0x000022b0  (PCC clock gate + source mux)
 *   commport_queue_receive      @ 0x00007550  (payload-queue receive)
 *   queue_ring_copyout          @ 0x000091ec  (ring copy-out helper)
 *   commport_teardown           @ 0x000079c0  (instance disable/teardown)
 *
 * See comm.h for the hardware model (4-instance serial IP, CAN-FD on instance 3,
 * eDMA+FIFO data path). The TX engine (0x7624), ring drain (0x2d34), IRQ
 * dispatch (0x2190), ISR install (0x78f0), RX callback (0x96ce) and the uncarved
 * FIFO ISR body (0x2510) are follow-ups.
 */

#include <stdint.h>

#include "comm.h"
#include "util.h"       /* vmem_copy */
#include "pcc.h"        /* pcc_gate_enable, port_clock_wait */
#include "hal.h"        /* irqn_to_gpio_index */

/* --- runtime globals / tables ------------------------------------------- */

/* Comm-port key registry, 8 slots (OEM 0x0000a558). */
extern uint32_t g_commport_registry[8];

/* Active comm-port handle slot (SRAM 0x20000698, *DAT_00007a44). */
extern void *g_commport_active;

/* PCC clock-mux argument tables, indexed by the peripheral's 0..8 index. */
extern const uint16_t pcc_gate_arg_table[];   /* 0x0001b2e0 (off-image)  */
extern const uint32_t port_clk_arg_table[];   /* 0x0000a534              */

/* Current-task TCB pointer (FreeRTOS), at SRAM 0x20000900 (*DAT_00007620). */
#define COMMQ_CUR_TCB   (*(void *volatile *)0x20000900u)

/* --- not-yet-translated VanMoof helpers (left in decompiler FUN_ form) --- */
extern uint32_t FUN_0000925a(commq_t *q);            /* bytes currently in ring */
extern void     FUN_00006dc4(uint32_t block_ms);     /* queue block/timeout setup */
extern void     FUN_000074e0(void *waiting_send);    /* release a waiting sender */
extern void     FUN_00003338(void *queue);           /* queue delete + free */
extern void     FUN_00006c70(void *list_item, uint32_t flags); /* event-list remove */

/* --- FreeRTOS / port primitives (vendor, deferred) ---------------------- */
extern void vPortRaiseBASEPRI(void);                 /* 0x0000950a (configASSERT) */
extern void vPortEnterCritical(void);                /* 0x00006454 */
extern void vPortExitCritical(void);                 /* 0x00006470 */
extern void vPortFree(void *pv);                     /* 0x00006b9c */
extern void FUN_0000636c(void);                      /* vTaskSuspendAll */
extern void FUN_0000694c(void);                      /* xTaskResumeAll  */

/*
 * commport_base_to_index — map a comm-port base to its instance index. // 0x00002c20
 *
 * Family base 0x40086000 step 0x1000 (DAT_00002c50): 0x86000->0, 87000->1,
 * 88000->2, 89000->3 (the CAN/CAN-FD instance). An unrecognised base falls
 * through to 0 (the OEM default; not an error sentinel).
 */
uint32_t commport_base_to_index(uint32_t base)
{
    uint32_t fam = 0x40086000u;                 /* DAT_00002c50 */

    if (base == fam)            return 0;       /* 0x40086000 */
    if (base == fam + 0x1000u)  return 1;       /* 0x40087000 */
    if (base == fam + 0x2000u)  return 2;       /* 0x40088000 */
    if (base == fam + 0x3000u)  return 3;       /* 0x40089000 */
    return 0;                                   /* OEM default fallthrough */
}

/*
 * commport_registry_index — linear search the 8-slot key registry. // 0x000024f4
 *
 * Returns the index 0..7 of the slot whose word equals `key`, or 8 if absent
 * (the OEM lets callers index one past the 8-entry tables on a miss). The
 * decompiler renders this void; the machine code returns the loop index in r0.
 */
int commport_registry_index(uint32_t key)
{
    int index = 0;
    const uint32_t *slot = g_commport_registry;  /* DAT_0000250c -> 0xa558 */

    do {
        if (key == *slot) {                      /* ldr.w r1,[r2],#4 ; cmp ; beq */
            return index;
        }
        index++;
        slot++;
    } while (index != 8);                         /* cmp r0,#8 ; bne */

    return index;                                 /* falls through with index == 8 */
}

/*
 * edma_tcd_build — build a 4-word eDMA Transfer Control Descriptor. // 0x00008a7a
 *
 * elem_size from the 2-bit size code at cfg[9:8]: code 2 -> 4, else code+1.
 * Validates addr_a/addr_b are non-zero exact multiples of elem_size; on failure
 * zeroes word0..word2 (word3 untouched). Otherwise writes the descriptor with
 * mult = elem_size * (cfg>>16) applied to the two 2-bit offset selectors.
 */
void edma_tcd_build(edma_tcd_t *tcd, uint32_t cfg, uint32_t addr_a,
                    uint32_t addr_b, uint32_t word3)
{
    uint32_t elem_size = (cfg >> 8) & 3u;        /* ubfx r4,r1,#8,#2 */

    if (elem_size == 2u) {                       /* ite eq ; mov.eq #4 / add.ne #1 */
        elem_size = 4u;
    } else {
        elem_size = elem_size + 1u;
    }

    if ((addr_a == 0u) || (addr_a != elem_size * (addr_a / elem_size)) ||
        (addr_b == 0u) || (addr_b != elem_size * (addr_b / elem_size))) {
        tcd->word1 = 0u;                         /* strd r3,r3,[r0,#0x4] */
        tcd->word2 = 0u;
        tcd->word0 = 0u;                         /* str r3,[r0,#0x0] */
    } else {
        uint32_t mult = elem_size * (cfg >> 16); /* lsrs r5,#0x10 ; muls */

        tcd->word0 = cfg;                        /* str r1,[r0,#0x0] */
        tcd->word1 = mult * ((cfg >> 12) & 3u) + addr_a;  /* mla r2,r4,r5,r2 */
        tcd->word2 = mult * ((cfg >> 14) & 3u) + addr_b;  /* mla r1,r4,r1,r3 */
        tcd->word3 = word3;                      /* ldr [sp,#0xc] ; str [r0,#0xc] */
    }
}

/*
 * edma_chan_irq_enable — set the eDMA channel-interrupt-enable bit. // 0x00008c46
 *
 * handle->edma_chan (+0x10) gives the eDMA base (+0x08) and channel (+0x0c). The
 * per-channel interrupt-enable group is at eDMA base+0x50, one 32-bit word per
 * 32 channels; RMW-set bit (ch & 31) in word (ch >> 5).
 */
void edma_chan_irq_enable(commport_handle_t *handle)
{
    edma_chan_desc_t *chan = handle->edma_chan;             /* [r0,#0x10] */
    uint8_t  ch       = chan->channel;                      /* [r2,#0x0c] */
    uint32_t base     = chan->edma_base;                    /* [r2,#0x08] */
    uint32_t word_idx = (uint32_t)(ch >> 5);
    volatile uint32_t *reg = (volatile uint32_t *)(base + 0x50);

    reg[word_idx] |= (uint32_t)1u << (ch & 0x1f);
}

/*
 * commport_uart_config — LPUART line/baud/FIFO config of one instance. // 0x00002754
 *
 * SBR = round((src_clk_hz)/target_baud) - 1 into BAUD[15:0]; CTRL/FORMAT/WATER
 * fields packed from the descriptor; per-instance byte tables stamped at the
 * instance index. Returns 0 ok, 4 invalid-arg, 0x15e3 baud-out-of-range.
 */
uint32_t commport_uart_config(uint32_t comm_base, const commport_uart_cfg_t *cfg,
                              uint32_t src_clk_hz)
{
    uint32_t target_baud = *(const uint32_t *)((const uint8_t *)cfg + 8); /* +0x08 */

    if (comm_base == 0 || target_baud == 0 || src_clk_hz == 0) {
        return 4;                                           /* invalid argument */
    }

    /* round-to-nearest SBR, minus one */
    uint32_t sbr = ((uint32_t)(src_clk_hz * 10) / target_baud + 5) / 10 - 1;
    if (sbr >= 0x10000) {
        return 0x15e3;                                      /* baud out of range */
    }

    volatile uint32_t *CTRL   = (volatile uint32_t *)(comm_base + 0x400);
    volatile uint32_t *FORMAT = (volatile uint32_t *)(comm_base + 0x404);
    volatile uint32_t *BAUD   = (volatile uint32_t *)(comm_base + 0x424);
    volatile uint32_t *FIFO   = (volatile uint32_t *)(comm_base + 0xe00);
    volatile uint32_t *WATER  = (volatile uint32_t *)(comm_base + 0xe08);

    *BAUD = *BAUD & 0xffff0000u;                             /* program SBR[15:0] */
    *BAUD = sbr | *BAUD;

    int idx = commport_registry_index(comm_base);           /* 0x24f4 */

    *CTRL = (((uint32_t)(cfg->f04 & 1) << 3)
             | (*CTRL & 0xfffff042u)
             | ((uint32_t)cfg->data_bits << 7)
             | ((uint32_t)cfg->f0e & 0xf00u)
             | ((uint32_t)(cfg->f03 & 1) << 4)
             | ((uint32_t)(cfg->f02 & 1) << 5)
             | 4u);

    /* per-instance byte-pair table (DAT_00002884 = 0x200070cf, stride 2) */
    *(volatile uint8_t *)(0x200070cfu + (uint32_t)idx * 2)     = cfg->f0c;
    *(volatile uint8_t *)(0x200070cfu + (uint32_t)idx * 2 + 1) = cfg->f0d;

    *FIFO = *FIFO | 0x30000u;                                /* FIFO enable/flush */
    *FIFO = *FIFO | 0x3u;

    *WATER = ((uint32_t)(cfg->f10 & 0xf) << 8)
             | ((uint32_t)(cfg->f11 & 0xf) << 16)
             | (*WATER & 0xfff0f0ffu)
             | 3u;

    *FORMAT = (uint32_t)(uint8_t)(cfg->f13 << 4)
              | ((uint32_t)(cfg->f14 & 0xf) << 8)
              | ((uint32_t)cfg->f12 & 0xfu)
              | (uint32_t)(uint16_t)((uint16_t)cfg->f15 << 12);

    /* mark this instance valid (DAT_00002888 = 0x200070e0, stride 1) */
    *(volatile uint8_t *)(0x200070e0u + (uint32_t)idx) = 0xff;

    if (cfg->enable != 0) {                                  /* final enable */
        *CTRL = *CTRL | 1u;
    } else {
        *CTRL = *CTRL & 0xfffffffeu;
    }
    return 0;
}

/*
 * peripheral_clock_mux_select — gate + select a peripheral's clock source. // 0x000022b0
 *
 * Enables the peripheral's clock gate (pcc_gate_enable) and PORT handshake
 * (port_clock_wait) by its 0..8 index, then validates the requested `source`
 * against the PCC control register at periph_base+0xff8: returns 3 if the source
 * is unavailable, 1 if a different source is already locked (CGC set), else
 * writes the source and returns 0. (Borderline SDK-derived clock glue.)
 */
int peripheral_clock_mux_select(uint32_t periph_base, uint32_t source)
{
    int idx = irqn_to_gpio_index((int)periph_base);          /* 0x20e4 */

    pcc_gate_enable(pcc_gate_arg_table[idx]);                /* 0x8a64 */
    port_clock_wait(port_clk_arg_table[idx]);                /* 0x84cc */

    volatile uint32_t *pcc = (volatile uint32_t *)(periph_base + 0xff8u);

    uint32_t ctrl = *pcc;                                    /* LOAD #1 */
    uint32_t avail;
    if (source == 5u) {
        avail = ctrl & 0x80u;                                /* tst.w #0x80 */
    } else {
        avail = (ctrl >> ((source + 3u) & 0xffu)) & 1u;
    }
    if (avail == 0u) {
        return 3;                                            /* source unavailable */
    }

    /* lock test: CGC (bit3) set AND current PCS != requested -> refuse.
     * The OEM re-reads +0xff8 for each test; preserved as separate volatile reads. */
    if (((*pcc << 0x1cu) & 0x80000000u) != 0u &&
        (*pcc & 7u) != source) {
        return 1;                                            /* already locked */
    }

    *pcc = source;
    return 0;
}

/*
 * queue_ring_copyout — copy min(count,avail) bytes out of a byte ring. // 0x000091ec
 *
 * Copies from ring->storage[ring->read] into `dst`, handling wrap at
 * ring->size, then advances and writes back ring->read. Returns the clamped
 * length. The configASSERT consistency checks spin forever on violation.
 */
unsigned int queue_ring_copyout(commq_t *ring, void *dst,
                                unsigned int count, unsigned int avail)
{
    unsigned int n;
    unsigned int rd;
    unsigned int cap;
    unsigned int first;

    n = avail;                                   /* n = min(count, avail) */
    if (count <= avail) {
        n = count;
    }

    if (n != 0) {
        rd  = ring->read;
        cap = ring->size;

        first = cap - rd;                        /* first = min(n, cap - rd) */
        if (n <= cap - rd) {
            first = n;
        }

        if (count < first) {                     /* configASSERT(count >= first) */
            vPortRaiseBASEPRI();
            for (;;) { }
        }
        if (cap < rd + first) {                  /* configASSERT(cap >= rd + first) */
            vPortRaiseBASEPRI();
            for (;;) { }
        }

        vmem_copy(dst, ring->storage + rd, first);

        if (first < n) {                         /* wrap */
            if (count < n) {                     /* configASSERT(count >= n) */
                vPortRaiseBASEPRI();
                for (;;) { }
            }
            vmem_copy((unsigned char *)dst + first, ring->storage, n - first);
        }

        rd += n;
        if (ring->size <= rd) {
            rd -= ring->size;
        }
        ring->read = rd;
    }

    return n;
}

/*
 * commport_queue_receive — receive one record from the payload queue. // 0x00007550
 *
 * Optionally blocks the calling task on the queue, then copies one record out of
 * the ring (via queue_ring_copyout) and wakes a waiting sender. Records carry an
 * optional 4-byte LE length prefix (when q->flags bit0 set), else a fixed 0x8c
 * payload. Returns the number of payload bytes copied (0 on empty/timeout/bad).
 */
int commport_queue_receive(commq_t *q, void *dst, uint32_t block_ms)
{
    uint32_t prefix_sz;
    uint32_t avail;
    uint32_t len = 0;
    uint32_t paylen;
    uint32_t saved_read;
    int      n;

    if (q == 0) {                                /* configASSERT(pxQueue) */
        vPortRaiseBASEPRI();
        for (;;) { }
    }

    prefix_sz = (q->flags & 1) ? 4u : 0u;

    if (block_ms != 0) {
        vPortEnterCritical();
        avail = FUN_0000925a(q);
        if (avail <= prefix_sz) {
            void *tcb = COMMQ_CUR_TCB;
            vPortEnterCritical();
            if (*((volatile uint8_t *)tcb + 0x68) == 2) {
                *((volatile uint8_t *)tcb + 0x68) = 0;
            }
            vPortExitCritical();
            if (q->rx_block != 0) {              /* configASSERT(rx_block == NULL) */
                vPortRaiseBASEPRI();
                for (;;) { }
            }
            q->rx_block = COMMQ_CUR_TCB;
        }
        vPortExitCritical();

        if (prefix_sz < avail) {
            goto have_data;
        }
        FUN_00006dc4(block_ms);                  /* block/timeout setup */
        q->rx_block = 0;
    }

    avail = FUN_0000925a(q);
    if (prefix_sz >= avail) {
        return 0;
    }

have_data:
    if (prefix_sz == 0) {
        paylen = 0x8c;                           /* fixed-size record */
    } else {
        saved_read = q->read;
        queue_ring_copyout(q, &len, prefix_sz, avail);   /* read length prefix */
        avail -= prefix_sz;
        paylen = len;
        if (len > 0x8c) {                        /* bogus length -> roll back */
            paylen = 0;
            q->read = saved_read;
        }
    }

    n = (int)queue_ring_copyout(q, dst, paylen, avail);
    if (n == 0) {
        return 0;
    }

    FUN_0000636c();                              /* vTaskSuspendAll */
    if (q->waiting_send != 0) {
        FUN_000074e0(q->waiting_send);
        q->waiting_send = 0;
    }
    FUN_0000694c();                              /* xTaskResumeAll */
    return n;
}

/*
 * commport_teardown — disable/teardown the comm-port instance. // 0x000079c0
 *
 * Acts only on the handle whose register base equals 0x4009d000 (DAT_00007a40,
 * the instance this teardown services): deletes the queue, walks + frees the
 * blocked-task list, and — if a reset is pending — software-resets the
 * peripheral (base+0x18) and gates its clock (PCC reg 0x40000244 = 0x80), then
 * clears the global active-handle slot.
 */
void commport_teardown(commport_handle_t *handle)
{
    if ((uint32_t)(uintptr_t)handle->base != 0x4009d000u) {   /* DAT_00007a40 */
        return;
    }

    if (handle->queue != 0) {
        FUN_00003338(handle->queue);                          /* queue delete */
    }

    {
        uint32_t list = (uint32_t)(uintptr_t)handle->evt_list;
        if (list != 0) {
            FUN_0000636c();                                   /* vTaskSuspendAll */
            while (*(volatile int *)(list + 4) != 0) {        /* uxNumberOfItems */
                uint32_t item = *(volatile uint32_t *)(list + 0x10);
                if (item == list + 0xc) {                     /* hit list end early */
                    vPortRaiseBASEPRI();                      /* configASSERT(0) */
                    for (;;) { }
                }
                FUN_00006c70((void *)(uintptr_t)item, 0x2000000u);
            }
            vPortFree((void *)(uintptr_t)list);
            FUN_0000694c();                                   /* xTaskResumeAll */
        }
    }

    if (handle->reset_pending != 0) {
        volatile uint32_t *base = handle->base;
        base[0x18 / 4] |= 1u;                                 /* assert SW reset */
        while ((int)(base[0x18 / 4] << 31) >= 0) { }          /* wait bit0 == 1 */
        base[0x18 / 4] |= 2u;                                 /* hold in reset */
        *(volatile uint32_t *)0x40000244u = 0x80u;            /* gate clock */
        handle->reset_pending = 0;
    }

    g_commport_active = 0;                                     /* clear active slot */
}

/* --- per-instance ISR registry tables ----------------------------------- */

/*
 * Per-instance ISR dispatch tables, indexed by the GPIO/instance index
 * returned by irqn_to_gpio_index() (0..8). The IRQ trampolines at 0x2100
 * fetch the bound handle/callback/arg from these slots.
 *
 *   g_isr_handle[idx]   (SRAM 0x200016a0, DAT_0000796c) — bound handle ptr
 *   g_isr_callback[idx] (SRAM 0x200016c4, DAT_00007970) — completion callback
 *   (the per-instance arg table at 0x200016b8 is written by the dispatch path)
 */
#define ISR_HANDLE_TABLE    ((void *volatile *)0x200016a0u)     /* DAT_0000796c */
#define ISR_CALLBACK_TABLE  ((void *volatile *)0x200016c4u)     /* DAT_00007970 */

/* per-instance byte-pair table (DAT_0000797c = 0x200070cf), stride 2; shared
 * with commport_uart_config which seeds f0c/f0d here. */
#define ISR_BYTEPAIR_TABLE  ((volatile uint8_t *)0x200070cfu)

/*
 * IRQ-number table (DAT_00007984 = 0x0001b464, off-image rodata), indexed by
 * the 0..7 registry index. Entries are SIGNED bytes: a negative value means
 * "no NVIC line" and is skipped by nvic_irq_enable.
 */
extern const int8_t commport_irqn_table[];                      /* 0x0001b464 */

/* Completion-callback pointer values stored verbatim into the callback table.
 * Thumb code-pointer constants (bit0 set); the dispatch path blx's them. */
#define CB_CAN_RX_COMPLETE  ((void *)0x00003111u)               /* DAT_00007974 */
#define CB_RX_COMPLETE      ((void *)0x000096e1u)               /* DAT_00007978: commport_rx_complete_cb */

/* Event/completion handler pointer stored at handle+0x1c. // DAT_00007980 */
#define ISR_EVENT_HANDLER   ((void *)0x000093cfu)

/*
 * commport_isr_install — register a comm-port instance's ISR state. // 0x000078f0
 *
 * ABI (verbatim from machine code at 0x78f0): (comm_base, handle_mem, arg).
 * Zeroes a 0x30-byte ISR-handle block at `handle_mem`, then:
 *   - binds the handle into g_isr_handle[gpio_idx]
 *   - selects the completion callback by the instance's CAN-mode flag
 *     ((comm_base+0x400) bit2): CAN -> 0x3111, else commport_rx_complete_cb
 *     (0x96e1) into g_isr_callback[gpio_idx]
 *   - copies the per-instance byte pair (table[reg_idx*2 + 0/1]) to +0x24/+0x25
 *   - seeds the cached FIFO levels from the FIFO status reg (base+0xe08):
 *       +0x2c <- RX fill  bits [11:8]    +0x2d <- TX fill  bits [19:16]
 *   - stores the event handler (0x93cf) at +0x1c and `arg` at +0x20 (one strd)
 *   - enables the instance's NVIC line from the signed-byte IRQ table.
 *
 * Both index helpers receive `comm_base` in r0 (the decompiler dropped the arg
 * to commport_registry_index, but the machine code keeps r0 live). The GPIO
 * index drives the handle/callback tables; the registry index drives the
 * byte-pair and IRQ-number tables — they are distinct.
 *
 * Returns 0 (movs r0,#0 ; the OEM ignores it).
 */
uint32_t commport_isr_install(uint32_t comm_base, void *handle_mem, void *arg)
{
    int reg_idx  = commport_registry_index(comm_base);   /* 0x24f4: r0=base */

    vmem_set(handle_mem, 0, 0x30);                        /* 0x9866 */

    int gpio_idx = irqn_to_gpio_index((int)comm_base);   /* 0x20e4: r0=base */

    /* CAN-mode select flag: (comm_base+0x400) bit2 (0xc00 bit-timing block). */
    uint32_t mode = *(volatile uint32_t *)(comm_base + 0x400);

    ISR_HANDLE_TABLE[gpio_idx]   = handle_mem;
    ISR_CALLBACK_TABLE[gpio_idx] = (mode & 4u) ? CB_CAN_RX_COMPLETE
                                               : CB_RX_COMPLETE;

    /* per-instance byte pair -> handle+0x24/+0x25 (seeded by commport_uart_config) */
    {
        const volatile uint8_t *pair = &ISR_BYTEPAIR_TABLE[reg_idx * 2];
        *((volatile uint8_t *)handle_mem + 0x24) = pair[0];
        *((volatile uint8_t *)handle_mem + 0x25) = pair[1];
    }

    /* cached FIFO fill levels from FIFO status reg (base+0xe08). */
    {
        uint32_t fifo = *(volatile uint32_t *)(comm_base + 0xe08);
        *((volatile uint8_t *)handle_mem + 0x2c) = (uint8_t)((fifo >> 8)  & 0xfu); /* RX fill [11:8]  */
        *((volatile uint8_t *)handle_mem + 0x2d) = (uint8_t)((fifo >> 16) & 0xfu); /* TX fill [19:16] */
    }

    /* strd: event handler @ +0x1c, arg @ +0x20. */
    *(void *volatile *)((uint8_t *)handle_mem + 0x1c) = ISR_EVENT_HANDLER;
    *(void *volatile *)((uint8_t *)handle_mem + 0x20) = arg;

    /* enable NVIC line; signed byte (ldrsb), negative => skipped by helper. */
    nvic_irq_enable((uint32_t)(int32_t)commport_irqn_table[reg_idx]);   /* 0x78d4 */

    return 0;
}
