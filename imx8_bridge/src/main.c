/*
 * main.c — VanMoof S5 imx8_bridge cold-start, task creation and heap.
 *
 *   vm_can_init       0x3ad8 : the bridge main() — LPC55 peripheral bring-up,
 *                              spawns the CAN/SPI tasks, starts the scheduler.
 *   queue_msg_enqueue 0x214c : the VanMoof task-create primitive (TCB + stack +
 *                              initial ARM exception frame, priority insert).
 *   heap_malloc       0x1f28 : first-fit split allocator over a 0x5400 pool.
 *
 * Translated from the OEM image (NXP LPC55S69, base 0x0). The FreeRTOS list
 * primitives, the NXP SDK clock/IOCON/DMA/M_CAN/FlexComm register bring-up and
 * MCAN_CalculateBitTimingParam are vendor; the VanMoof glue (clock-enable
 * bitmasks, task creation, CAN-session/SPI-channel construction, scheduler
 * hand-off) is reconstructed. vm_can_init is behaviour-oriented: it keeps the
 * OEM phase order, constants and the fatal diagnostic codes, while the lengthy
 * SDK register sequences are summarised at their call sites.
 */

#include "imx8_bridge.h"
#include <string.h>

/* ============================================================== heap (0x1f28) */

#define HEAP_SIZE   0x5400u
#define HEAP_USED   0x80000000u           /* block[1] in-use flag */

static uint8_t   g_heap[HEAP_SIZE];       /* DAT_2048 backing buffer (BSS) */
static uint32_t *g_free_head;             /* DAT_204c free-list anchor */
static int       g_heap_inited;           /* DAT_2040 */
static uint32_t  g_min_free, g_cur_free;  /* DAT_2050 / DAT_2054 */
static uint32_t  g_alloc_count;           /* DAT_2058 */

static void heap_init(void)
{
    uintptr_t start = ((uintptr_t)g_heap + 7u) & ~(uintptr_t)7u;
    uintptr_t end   = ((uintptr_t)g_heap + HEAP_SIZE - 8u) & ~(uintptr_t)7u;
    uint32_t *blk   = (uint32_t *)start;
    uint32_t *sent  = (uint32_t *)end;

    sent[0] = 0;                          /* end-of-list sentinel */
    sent[1] = 0;
    blk[0]  = (uint32_t)(uintptr_t)sent;  /* fwd ptr -> sentinel */
    blk[1]  = (uint32_t)(end - start);    /* size */
    g_free_head  = blk;
    g_cur_free   = g_min_free = (uint32_t)(end - start);
    g_heap_inited = 1;
}

/* First-fit, split-with-remainder allocator. Returns an 8-byte-aligned, zeroed
 * payload pointer (past the 2-word header), or NULL when no block fits. */
void *heap_malloc(size_t size)
{
    uint32_t need;
    uint32_t *prev, *blk;

    if (!g_heap_inited)
        heap_init();

    if (size == 0 || size > 0xfffffff5u)
        return NULL;

    need = (uint32_t)((size + 8u + 7u) & ~7u);   /* header + 8-byte align */

    prvIncrementSuspendedCounter();              /* suspend IRQs around the walk */

    prev = g_free_head;
    blk  = g_free_head;
    while (blk != NULL && blk[1] < need) {
        prev = blk;
        blk  = (uint32_t *)(uintptr_t)blk[0];
        if (blk != NULL && blk[1] == 0)          /* hit the sentinel */
            blk = NULL;
    }
    if (blk == NULL) {
        xTimerGenericCommand(NULL);              /* re-enable IRQs */
        return NULL;
    }

    /* unlink, optionally splitting the remainder back onto the free list */
    if (blk[1] - need > 0x10u) {
        uint32_t *rem = (uint32_t *)((uint8_t *)blk + need);
        rem[0] = blk[0];
        rem[1] = blk[1] - need;
        prev[0] = (uint32_t)(uintptr_t)rem;
        blk[1] = need;
    } else {
        prev[0] = blk[0];
    }

    blk[1] |= HEAP_USED;                          /* mark in use */
    blk[0]  = 0;
    g_cur_free -= need;
    if (g_cur_free < g_min_free)
        g_min_free = g_cur_free;
    g_alloc_count++;

    xTimerGenericCommand(NULL);                   /* re-enable IRQs */

    if (((uintptr_t)(blk + 2) & 7u) != 0) {       /* alignment trap */
        port_set_interrupt_mask();
        for (;;) { }
    }
    mem_set(blk + 2, 0, size - 8u);
    return blk + 2;
}

/* ====================================================== task-create (0x214c) */

/* The VanMoof FreeRTOS task spawn: allocates a stack + a 0x6c-byte TCB, copies
 * the name, builds the initial Cortex-M exception frame so the first context
 * restore enters `task_fn(arg)`, and inserts the TCB into the priority-ordered
 * ready list. Returns 1, or -1 on allocation failure. */
int queue_msg_enqueue(task_fn_t task_fn, const char *name, int stack_words, void *arg,
                      int priority, void **out_tcb)
{
    extern void  *g_lr_sentinel;          /* DAT_2300 — task return-address sentinel */
    extern uint32_t g_task_count;         /* DAT_2304 */
    extern void  *g_ready_head;           /* DAT_2308 */
    extern uint32_t g_ready_lists[6][5];  /* DAT_230c — 6 priority buckets */
    extern uint32_t g_enq_count;          /* DAT_232c */
    extern uint32_t g_max_prio;           /* DAT_2330 */
    extern uint32_t g_sched_running;      /* DAT_2334 */

    uint32_t *stack = (uint32_t *)heap_malloc((size_t)stack_words * 4u);
    uint32_t *node;
    uint32_t *sp;
    int i;

    if (stack == NULL)
        return -1;
    node = (uint32_t *)heap_malloc(0x6c);
    if (node == NULL)
        return -1;

    /* node fields */
    strncpy((char *)node + 0x34, name, 20);                  /* +0x34 name */
    *((char *)node + 0x47) = 0;
    node[0x30 / 4] = (uint32_t)(uintptr_t)stack;             /* +0x30 stack ptr */
    node[0x2c / 4] = (uint32_t)priority;                     /* +0x2c priority */
    node[0x48 / 4] = (uint32_t)priority;                     /* +0x48 priority copy */
    node[0x18 / 4] = (uint32_t)(5 - priority);               /* +0x18 inverse prio */
    node[0x10 / 4] = (uint32_t)(uintptr_t)(node + 0x10 / 4); /* +0x10 self list-item */
    node[0x24 / 4] = (uint32_t)(uintptr_t)(node + 0x24 / 4); /* +0x24 self */
    node[0x14 / 4] = 0;
    node[0x28 / 4] = 0;
    node[0x4c / 4] = 0;

    /* initial exception frame at the 8-aligned top of the stack */
    sp = (uint32_t *)(((uintptr_t)stack + (size_t)stack_words * 4u) & ~(uintptr_t)7u);
    sp[-1] = 0x01000000u;                         /* xPSR (Thumb) */
    sp[-2] = (uint32_t)(uintptr_t)task_fn;         /* PC */
    sp[-3] = (uint32_t)(uintptr_t)g_lr_sentinel;   /* LR */
    sp[-4] = 0x12121212u;                          /* r12 */
    sp[-5] = 0x03030303u;                          /* r3 */
    sp[-6] = 0x02020202u;                          /* r2 */
    sp[-7] = 0x01010101u;                          /* r1 */
    sp[-8] = (uint32_t)(uintptr_t)arg;             /* r0 */
    sp[-9]  = 0x11111111u;                         /* r11 */
    sp[-10] = 0x10101010u;                         /* r10 */
    sp[-11] = 0x09090909u;                         /* r9 */
    sp[-12] = 0x08080808u;                         /* r8 */
    sp[-13] = 0x07070707u;                         /* r7 */
    sp[-14] = 0x06060606u;                         /* r6 */
    sp[-15] = 0x05050505u;                         /* r5 */
    sp[-16] = 0x04040404u;                         /* r4 */
    sp[-17] = 0xfffffffdu;                          /* EXC_RETURN (thread, PSP) */
    node[0] = (uint32_t)(uintptr_t)(sp - 17);      /* +0x00 initial SP */

    *out_tcb = node;

    vTaskEnterCritical();
    g_task_count++;
    if (g_ready_head == NULL) {
        g_ready_head = node;
        if (g_task_count == 1)
            for (i = 0; i < 6; i++)
                vListInitialise(g_ready_lists[i]);
    }
    if ((uint32_t)priority > g_max_prio)
        g_max_prio = (uint32_t)priority;
    g_enq_count++;
    vListInsertEnd(g_ready_lists[priority], node + 1);
    vTaskExitCritical();

    if (g_sched_running != 0 /* && higher prio than current */)
        port_yield_pend_sv();
    return 1;
}

/* ============================================================ main (0x3ad8) */

/* Fatal diagnostic codes emitted via can_tx_send_msg on an unrecoverable init
 * failure (then the function spins forever). */
enum {
    ERR_RX_HANDLE = 0x3f, ERR_SPI_DESC = 0x45, ERR_TX_SLOT = 0x4b,
    ERR_FLEXCOMM  = 0x4d, ERR_TX_SLOT2 = 0x54, ERR_MCAN_DRV = 0x56,
    ERR_MB_GROUP  = 0x5c, ERR_STREAMBUF = 0x65, ERR_PLL_LOCK = 0x68,
    ERR_FLASH_TBL = 0x77, ERR_FLASH_INIT = 0x7b
};

static NORETURN void init_fatal(uint8_t code)
{
    can_tx_send_msg(0, code, 0);
    for (;;) { }
}

/* The bridge main(): brings up the LPC55S69 peripherals, builds the channel /
 * session state, spawns the CAN/SPI worker tasks, and hands control to the
 * FreeRTOS scheduler. Behaviour-oriented; the SDK register bring-up is summarised
 * at its call sites and the VanMoof glue is reconstructed in OEM phase order. */
void vm_can_init(void)
{
    void *can_task_tcb, *cantx_task_tcb, *timer_task_tcb;
    void *rx_handle, *dma_ring, *qmsg, *mcan_drv, *spi_desc, *tx_slot;
    void *spi_chan;
    static void *can_slot0_handle;       /* persistent CAN session handle */
    static void *timer_handle;

    /* (1) enable the FlexComm / DMA / GPIO AHB clocks (SYSCON AHBCLKCTRL0). */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x800u;     /* FC0  */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x2000u;    /* FC1  */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x4000u;    /* FC2  */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x8000u;    /* FC3  */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x40000u;   /* DMA  */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x80u;      /* FC SPI */
    MMIO32(SYSCON_AHBCLKCTRL0) |= 0x400000u;  /* PLL gate */

    /* (2) per-peripheral clock enables: MCAN0 (0xe), MCAN1 (0xf x2). */
    clock_enable_peripheral(0xe);
    clock_enable_peripheral(0xf);
    clock_enable_peripheral(0xf);

    /* (3) IOCON pin-mux: PIO0_2/3/4/6/9 (CAN) + PIO0_33/34/35/53 (SPI), func=1.
     * (4) low-level M_CAN reset/enable. */
    can_fd_hw_init(NULL);

    /* (5..6) DMA0 descriptor table + GPIO bring-up (SDK). */

    /* (7) the master state block that holds all channel/session metadata. */
    {
        uint8_t *S = (uint8_t *)heap_malloc(0x5b0);
        if (S == NULL) init_fatal(ERR_RX_HANDLE);
        *(uint32_t *)(S + 0x04) = 0x20;
        *(uint32_t *)(S + 0x0c) = (uint32_t)(uintptr_t)(S + 0x10);
    }

    /* (8) the 8-byte CAN-RX handle + its 0xb40-byte DMA ring buffer. */
    rx_handle = heap_malloc(8);
    if (rx_handle == NULL) init_fatal(ERR_RX_HANDLE);
    dma_ring = heap_malloc(0xb40);
    ((void **)rx_handle)[0] = dma_ring;

    /* (9) the CAN-RX worker task ('can' = can_rx_dispatch_loop), priority 2. */
    qmsg = heap_malloc(0xc);
    ((void **)rx_handle)[1] = qmsg;
    queue_msg_enqueue((task_fn_t)can_rx_dispatch_loop, "can", 0x168, rx_handle, 2, &can_task_tcb);

    /* (10..12) MCAN driver block + SPI channel descriptor + a pending TX slot. */
    mcan_drv = heap_malloc(0xe4);
    if (mcan_drv == NULL) init_fatal(ERR_MCAN_DRV);
    spi_desc = heap_malloc(0x60);
    if (spi_desc == NULL) init_fatal(ERR_SPI_DESC);
    spi_chan = spi_channel_create();
    tx_slot  = can_tx_slot_alloc();
    if (tx_slot == NULL) init_fatal(ERR_TX_SLOT);
    if (heap_malloc(0x6a4) == NULL) init_fatal(ERR_TX_SLOT2);  /* TX FIFO descriptor */
    if (heap_malloc(0x17c) == NULL) init_fatal(ERR_MCAN_DRV);  /* second MCAN driver */
    (void)spi_desc; (void)spi_chan; (void)mcan_drv;

    /* (13) read SYSCON.DEVICE_ID to pick the oscillator for the CAN prescaler. */
    (void)MMIO32(SYSCON_DEVICE_ID0);

    /* (14) the SPI-slave FlexComm channel + the SPI-TX worker ('CanTX' =
     * spi_tx_send_loop), priority 3. */
    {
        spi_channel_t *sch = spi_channel_alloc(0x80, 0x10);
        if (sch == NULL) init_fatal(ERR_FLEXCOMM);
        queue_msg_enqueue((task_fn_t)spi_tx_send_loop, "CanTX", 0x10e, sch, 3, &cantx_task_tcb);
    }

    /* (15..16) prime the first CAN mailbox, register the 'vm' channel-map entry,
     * and open the per-ECU CAN session (200 ms timeout). */
    if (can_mb_group_isr(NULL, 0x7fffffff) != 0) init_fatal(ERR_MB_GROUP);
    can_session_open(&can_slot0_handle, 200, 0, 0);

    /* (19) the session-timer task (priority 0) + persistent config + a second
     * SPI-TX task (priority 4). */
    flash_read_sector(0);
    can_session_open(&timer_handle, 5000, 1, 0);
    queue_msg_enqueue((task_fn_t)spi_tx_send_loop, "vm", 0x5a, NULL, 0, &timer_task_tcb);

    /* (20) SysTick: 1  kHz tick, lowest IRQ priority. */
    MMIO32(SCB_SHPR3) |= 0xffff0000u;
    MMIO32(SYST_RVR)   = 0;                 /* = mainclk/1000 - 1 (SDK) */
    MMIO32(SYST_CVR)   = 0;
    MMIO32(SYST_CSR)   = 7;

    /* (21) hand off to the FreeRTOS scheduler; never returns. */
    vPortStartFirstTask();
    for (;;) { }
}
