/*
 * main.c — VanMoof S5 motor_sensor: cold-start, peripheral bring-up, scheduler.
 *
 *   motor_sensor_hw_init_and_scheduler_start 0x375c : the app main()
 *   peripheral_hw_init                       0x229e : 2nd-stage peripheral init
 *   can_controller_init                      0x2d64 : M_CAN reset + filters
 *
 * Translated from the OEM image (LPC546xx-class, base 0x0). The NXP LPC SDK/HAL
 * register bring-up (clocks/IOCON/M_CAN/ADC/SCT) and FreeRTOS are vendor; the
 * VanMoof glue (clock-gate sequence, task creation, CAN subscriptions, sensor
 * MMIO scaling, scheduler hand-off) is reconstructed. Behaviour-oriented: OEM
 * phase order + constants are kept; the verifier's corrections are folded in
 * (first AHBCLKCTRLSET write is 0x800; M_CAN base is 0x40006000; the FIFO-ready
 * wait bit is 24; the clock probe reads 0x400002A0; SCT base is 0x40008000).
 */

#include "motor_sensor.h"

/* config-pointer literals (DAT_3bXX) + clock/sensor globals. */
extern void    *g_sub_ctx;          /* DAT_3b54 — main state/subscription block */
extern uint32_t g_pll_ref;          /* DAT_3b20 — SPI clock-divider denominator */
extern sensor_regs_t *g_sensor_regs;/* DAT_4924 — sensor MMIO block @0xf400 */
extern uint32_t CLOCK_GetSysFreq(void);  /* derived from the 0x400002A0 probe */
extern void    *g_motor_task_arg;   /* xTaskCreate arg block */

/* ------------------------------------------------------------------- 0x2d64 */

/* M_CAN controller reset + acceptance-filter init. */
uint32_t can_controller_init(uint32_t a, uint32_t b)
{
    can_ctrl_state_t *st = (can_ctrl_state_t *)0x1301FE00u;   /* _LAB_2dbc */
    (void)a; (void)b;

    st->init_busy = 1;
    MMIO32(SYSCON_PERIPH_PD) = 0;                    /* 0x4000038c: force CAN reset */

    /* a 5000 ms auto-reload recovery timer drives bus-error recovery */
    xTimerCreate_wrap(NULL, 5000, 1, NULL, NULL);

    can_bus_error_recover();                          /* restore clock gate + release reset */
    mcan_lowlevel_init();                             /* 0x15fc: CCCR/NBTP/DBTP/msg-RAM */

    /* filter list size 1, start address 1 (SIDFC / XIDFC) */
    MMIO32(MCAN_BASE + MCAN_SIDFC) = (MMIO32(MCAN_BASE + MCAN_SIDFC) & ~0xffffffu) | 0x101u;
    MMIO32(MCAN_BASE + MCAN_XIDFC) = (MMIO32(MCAN_BASE + MCAN_XIDFC) & ~0xffffffu) | 0x101u;

    st->tx_done_en = 0;                               /* handed to peripheral_hw_init */
    return 0;
}

/* ------------------------------------------------------------------- 0x229e */

/* Second-stage peripheral init (M_CAN filters, secondary SPI, ADC, SCT, PLL),
 * run from a timer/SysTick callback context. */
uint32_t peripheral_hw_init(void)
{
    uint8_t *ctx = (uint8_t *)0x40006000u;            /* _LAB_247c view */

    *(uint8_t *)(0x1301FE00u + 0x21) = 1;             /* enable CAN TX-done */
    MMIO32(MCAN_BASE + MCAN_SIDFC) |= 0x100u;         /* msg-RAM start addr */
    MMIO32(MCAN_BASE + MCAN_XIDFC) |= 0x100u;
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x800u;              /* gate FLEXCOMM0 */
    MMIO32(SYSCON_AHBCLKCTRL1) = 0x800u;

    /* secondary SPI (0x40004000) FIFO/CFG/INTENSET */
    MMIO32(SPI2_BASE + 0x00) = 0x20u;
    MMIO32(SPI2_BASE + 0x18) = 0;
    *(uint32_t *)(0x1301FE00u + 0x184) = 2;           /* state-machine word */
    NVIC_EnableIRQ(0x21);                             /* IRQ 33 = SPI2/ADC-done */

    /* ADC enable + prescaler */
    MMIO32(SYSCON_BASE + 0x40) |= 3u;
    MMIO32(SYSCON_BASE + 0xd4) |= 3u;
    clock_prescaler_config(0);

    MMIO32(SYSCON_BASE + 0x380) = 0;                  /* SYSPLLCTRL reset */
    MMIO32(SYSCON_BASE + 0x400) = 0x201au;            /* SCT CONFIG: UNIFY|NORELOAD|INSYNC */
    (void)ctx;
    return 0;
}

/* ------------------------------------------------------------------- 0x375c */

/* The motor_sensor app main(): bring up the LPC peripherals, register the CAN
 * subscriptions, spawn the tasks, and start the scheduler (never returns). */
void motor_sensor_hw_init_and_scheduler_start(void)
{
    void *task_hdr, *task_buf, *t;
    uint32_t sysfreq;

    /* (1) clock gates (write-1-to-set; first write is 0x800, GPIO0). */
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x800u;    /* GPIO0 */
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x2000u;   /* FLEXCOMM2 */
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x4000u;   /* FLEXCOMM3 */
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x8000u;   /* FLEXCOMM7/SPI */

    /* (2) GPIO pin config (P0_2/4/5/6/9/10, P1_1). (3) IOCON pin-mux is at
     * MCAN_BASE-0x5000 = 0x40001000; the write to [0x40006000+0x20] is the
     * M_CAN TSCC register, not an IOCON enable. */
    gpio_set_pin(0, 2, NULL); gpio_set_pin(0, 4, NULL); gpio_set_pin(0, 5, NULL);
    gpio_set_pin(0, 6, NULL); gpio_set_pin(0, 9, NULL); gpio_set_pin(0, 10, NULL);
    gpio_set_pin(1, 1, NULL);

    /* (4) M_CAN acceptance-filter / controller init. */
    can_controller_init(0, 0);

    /* (5) CAN bit-timing/FIFO bring-up at CAN_TIMING_BASE (NBTP/DBTP/SSP/GFC);
     * FIFO-ready wait is on bit 24. (6) probe SYSCON 0x400002A0 for the clock. */
    MMIO32(SYSCON_AHBCLKCTRL0) = 0x8000000u;          /* enable FLEXCOMM7 clock */
    while (!(MMIO32(CAN_TIMING_BASE + 0xf0) & 0x1000000u)) { }  /* bit 24 */
    MMIO32(CAN_TIMING_BASE + MCAN_GFC) = 0;            /* global filter config */
    sysfreq = CLOCK_GetSysFreq();                      /* via 0x400002A0 / AHBCLKDIV */
    NVIC_EnableIRQ(0x16);                              /* IRQ 22 = SCT/timer */

    /* (7) main state block + (8) the motor_sensor task. */
    g_sub_ctx = g_sub_ctx;                             /* DAT_3b54 — already alloc'd */
    task_hdr = pvPortMalloc(8);
    if (task_hdr == NULL) for (;;) { }
    task_buf = pvPortMalloc(0xb61);                    /* FreeRTOS task buffer */
    if (task_buf == NULL) for (;;) { }
    mem_set((uint8_t *)task_buf + 0x20, 0x55, 0xb41);  /* stack-fill sentinel */
    mem_set(task_buf, 0, 0x20);
    xTaskCreate((void (*)(void *))motor_sensor_task_loop, "motor_sensor_task",
                0x168, g_motor_task_arg, 2, &t);

    /* (9) register the CAN subscriptions + send the initial version frame. */
    can_diag_log_send(0x18, 4, 1, 0x35);              /* version report (cmd 0x35) */

    /* (10) spawn the CAN tasks. */
    xTaskCreate((void (*)(void *))can_tx_task,        "CanTX", 0x10e, NULL, 3, &t);
    xTaskCreate((void (*)(void *))can_tx_manager_loop,"can",   0x186, NULL, 2, &t);

    /* (11) two ADC reads, scaled by the FPU, written to the sensor MMIO block
     * under a 0xAA/0x55 unlock. */
    {
        uint32_t pm = port_set_interrupt_mask();
        g_sensor_regs->wdt_key     = 0xaa;
        g_sensor_regs->wdt_key     = 0x55;
        g_sensor_regs->current_u24 = (uint32_t)((float)g_sensor_regs->adc_10bit * 5.75f);
        g_sensor_regs->voltage_u24 = (uint32_t)((float)g_sensor_regs->adc_10bit * 1.5f);
        port_clear_interrupt_mask(pm);
    }

    /* (12) the 5 s watchdog timer + the final/watchdog tasks. */
    watchdog_timer_start(NULL, 0, 0);
    xTaskCreate((void (*)(void *))can_tx_manager_loop, "vm",   0x5a,  NULL, 0, &t);
    xTaskCreate((void (*)(void *))watchdog_timer_start,"VMKYS",0x10e, NULL, 4, &t);

    /* (13) SysTick (1 kHz) + scheduler hand-off (never returns). */
    MMIO32(SCB_SHPR3) |= 0xffff0000u;
    MMIO32(SYST_RVR)   = sysfreq / 1000u - 1u;
    MMIO32(SYST_CSR)   = 7u;
    vPortStartFirstTask();
    for (;;) { }
}
