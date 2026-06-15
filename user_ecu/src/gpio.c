/*
 * gpio.c - GPIO pin configuration + GPIO interrupt-dispatch glue.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M Cortex-M4F, FreeRTOS). All MMIO is reproduced verbatim as
 * volatile pointer accesses at their absolute addresses; that is VanMoof's
 * own bare-metal glue.
 */

#include <stdint.h>
#include <stddef.h>

#include "gpio.h"
#include "hal.h"   /* int irqn_to_gpio_index(int irqn); */
#include "pcc.h"   /* pcc_gate_enable, gpio_base_to_bank, port_clock_wait */

/* ---- cross-module externs (declared, not reconstructed) --------------- */

/*
 * Runtime GPIO IRQ handler table walked by gpio_irq_dispatch (SRAM 0x200014ac).
 *
 * The bank index (0/1/2) selects a 0x5c-byte window into the table; from
 * there gpio_irq_dispatch reads 23 (0x17) consecutive 4-byte slots, each a
 * pointer to a handler record (or NULL). Each handler record:
 *   +0x00  void (*handler)(void *self, void *arg, uint32_t edge, uint32_t dir)
 *   +0x04  void *arg
 *   +0x08  volatile uint8_t *port_base  (PORT regs; status @ +0x40/+0x58/+0x60)
 *   +0x0c  uint8_t pin   (bits [7:5] = status-reg subindex, [4:0] = bit number)
 */
extern uint8_t g_gpio_irq_table[];                     /* SRAM 0x200014ac */

/*
 * Runtime per-bank dispatch tables used by the bank trampolines (indexed by
 * the 0..8 value returned from irqn_to_gpio_index):
 *   g_gpio_bank_arg0  : first callback argument (OEM table at rodata 0x0000a510)
 *   g_gpio_bank_arg1  : second callback argument (SRAM 0x200016a0)
 *   g_gpio_bank_isr   : callback function pointer  (SRAM 0x200016c4)
 */
extern uint32_t          g_gpio_bank_arg0[9];          /* 0x0000a510 */
extern uint32_t          g_gpio_bank_arg1[9];          /* 0x200016a0 */
extern void (*const      g_gpio_bank_isr[9])(uint32_t arg0, uint32_t arg1);

/* Tables referenced by gpio_bank_irq_trampoline_9 (the pin-config routine). */
extern const uint16_t    g_port_pcc_index[9];          /* 0x0001b2e0 (data) */
extern const uint32_t    g_port_mux_arg[9];            /* 0x0000a534 */

/* ----------------------------------------------------------------------- */

/* GPIO output register block base. // literal @0x000065d8 -> 0x4008E000 */
#define GPIO_OUT_BASE  0x4008E000u

/*
 * Configure a single GPIO output pin. // 0x000065a0
 *
 * Word indices used by the OEM (verbatim):
 *   reg[0x00] = DATA       (+0x000)
 *   reg[0x80] = SET-aux    (+0x200)
 *   reg[0xa0] = CLEAR-aux  (+0x280)
 */
void gpio_pin_config(uint32_t pin, const char *flags)
{
    volatile uint32_t *gpio = (volatile uint32_t *)GPIO_OUT_BASE;
    uint32_t mask;

    pcc_gate_enable(0xe); /* enable the GPIO peripheral clock gate */

    mask = 1u << (pin & 0xff);

    if (flags[0] == '\0') {
        /* clear the pin's bit in the data register */
        gpio[0] = gpio[0] & ~mask;
    } else {
        if (flags[1] == '\0') {
            gpio[0xa0] = mask;   /* +0x280 */
        } else {
            gpio[0x80] = mask;   /* +0x200 */
        }
        gpio[0] = mask | gpio[0];
    }
}

/*
 * Walk the active bank's handler table and dispatch set interrupt bits.
 * // 0x00002018
 *
 * The bank is selected by FUN_00001ffc() (returns 0/1/2); the table base is
 * advanced by bank*0x5c so each bank occupies its own slice of the 23 records.
 */
void gpio_irq_dispatch(void)
{
    int bank = (int)gpio_base_to_bank(0); /* phantom arg: r0 is undefined at entry,
                                 * see open_issues; OEM passes whatever is in r0 */
    uint8_t *rec = g_gpio_irq_table + (uint32_t)bank * 0x5c;
    int i = 0;

    do {
        /* +0x00: handler pointer (record treated as a word array) */
        uintptr_t *self = *(uintptr_t **)rec;

        if (self != NULL) {
            uint8_t pin   = *(uint8_t *)(self + 3);       /* +0x0c */
            uint32_t sub  = (uint32_t)(pin >> 5);         /* status subindex */
            uint32_t bit  = pin & 0x1f;                   /* bit number */
            uintptr_t port = self[2];                     /* +0x08 PORT base */

            void (*cb)(uintptr_t *, uintptr_t, uint32_t, uint32_t) =
                (void (*)(uintptr_t *, uintptr_t, uint32_t, uint32_t))self[0];

            /* status reg +0x58 (rising) */
            volatile uint32_t *s58 = (volatile uint32_t *)(port + 0x58 + sub * 4);
            if ((int)((*s58 >> bit) << 31) < 0) {
                *s58 = 1u << bit;                         /* W1C */
                if (cb != NULL) {
                    cb(self, self[1], 1, 0);
                }
            }

            /* status reg +0x60 (falling) */
            volatile uint32_t *s60 = (volatile uint32_t *)(port + 0x60 + sub * 4);
            if ((int)((*s60 >> bit) << 31) < 0) {
                *s60 = 1u << bit;                         /* W1C */
                if (cb != NULL) {
                    cb(self, self[1], 1, 1);
                }
            }

            /* status reg +0x40 (level/other) */
            volatile uint32_t *s40 = (volatile uint32_t *)(port + 0x40 + sub * 4);
            if ((int)((*s40 >> bit) << 31) < 0) {
                *s40 = 1u << bit;                         /* W1C */
                if (cb != NULL) {
                    cb(self, self[1], 0, 2);
                }
            }
        }

        rec += 4; /* OEM advances by one word per iteration (ldr rX,[r6],#4) */
        i++;
    } while (i != 0x17);
}

/* ----------------------------------------------------------------------- */
/* GPIO bank IRQ trampolines. // 0x00002100 .. 0x00002280                  */
/*                                                                         */
/* Each is byte-identical save for the bank base it feeds to               */
/* irqn_to_gpio_index(). They tail-call the per-bank handler:              */
/*   idx = irqn_to_gpio_index(BANK_BASE);                                  */
/*   g_gpio_bank_isr[idx](g_gpio_bank_arg0[idx], g_gpio_bank_arg1[idx]);   */
/* ----------------------------------------------------------------------- */

#define DEFINE_GPIO_BANK_TRAMPOLINE(suffix, bank_base)                     \
    void gpio_bank_irq_trampoline_##suffix(void)                           \
    {                                                                      \
        int idx = irqn_to_gpio_index((int)(bank_base));                    \
        g_gpio_bank_isr[idx](g_gpio_bank_arg0[idx], g_gpio_bank_arg1[idx]);\
    }

DEFINE_GPIO_BANK_TRAMPOLINE(0, 0x40086000) /* 0x00002100 */
DEFINE_GPIO_BANK_TRAMPOLINE(1, 0x40087000) /* 0x00002130 */
DEFINE_GPIO_BANK_TRAMPOLINE(2, 0x40088000) /* 0x00002160 */
DEFINE_GPIO_BANK_TRAMPOLINE(3, 0x40089000) /* 0x00002190 */
DEFINE_GPIO_BANK_TRAMPOLINE(4, 0x4008A000) /* 0x000021c0 */
DEFINE_GPIO_BANK_TRAMPOLINE(5, 0x40096000) /* 0x000021f0 */
DEFINE_GPIO_BANK_TRAMPOLINE(6, 0x40097000) /* 0x00002220 */
DEFINE_GPIO_BANK_TRAMPOLINE(7, 0x40098000) /* 0x00002250 */
DEFINE_GPIO_BANK_TRAMPOLINE(8, 0x4009F000) /* 0x00002280 */

/*
 * gpio_bank_irq_trampoline_9 // 0x000022b0
 *
 * NOT a trampoline: this is the per-pin interrupt-configuration routine
 * (called from 0x00005e30, not from a vector). Named per the module spec.
 *
 *   port_base : PORT/GPIO base of the pin being configured.
 *   pin_sel   : interrupt mode/selector (compared against 5, masked to 3 bits).
 *
 * Returns: 0 = configured ok, 1 = conflict (already locked to another mode),
 *          3 = the requested mode bit is not enabled.
 */
int gpio_bank_irq_trampoline_9(volatile uint32_t *port_base, uint32_t pin_sel)
{
    int idx = irqn_to_gpio_index((int)(uintptr_t)port_base);

    /* enable PORT clock gate for this bank, then run the mux/clock helper */
    pcc_gate_enable(g_port_pcc_index[idx]);
    port_clock_wait(g_port_mux_arg[idx]);

    /* status word at port_base + 0xff8 */
    volatile uint32_t *st = (volatile uint32_t *)((uintptr_t)port_base + 0xff8);
    uint32_t enabled;

    if (pin_sel == 5) {
        enabled = *st & 0x80;
    } else {
        enabled = (*st >> ((pin_sel + 3) & 0xff)) & 1;
    }

    if (enabled == 0) {
        return 3;
    }

    if (((int)(*st << 0x1c) < 0) && ((*st & 7) != pin_sel)) {
        return 1;
    }

    *st = pin_sel;
    return 0;
}
