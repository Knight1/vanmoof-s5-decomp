/*
 * gpio.c -- VanMoof S5 motor_control (GPIO pin-mux + config).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== gpio_set_pin_attributes  (OEM 0x082964, gpio) =====
 * EALLOW-protected GPIO attribute writer. Given a packed pin descriptor in ACC (bits [15:5] = pin-group index, bits [4:0] = pin number within group) and a flags byte in *XAR6 (bit0=pull-up-disable, bit1=open-drain, bit2=analog-mode), computes the GPIO ctrl block pointer via a lookup table and SET/CLR the GPAPUD (+12), GPAODR (+16), and GPAAMSEL (+18) register pairs. Called by 41 sites during periphe
 * Peripherals: GPIO ctrl 0x7C00 (word base); lookup table indexed by (pin_desc>>4)&~1; GPAPUD offset+12, GPAODR offset+16, GPAAMSEL offset+18
 * [confidence: medium] */
/* 0x082964 - gpio_set_pin_attributes */
void gpio_set_pin_attributes(uint32_t pin_desc, uint16_t flags)
{
    /* pin_desc: [15:5]=group_index, [4:0]=pin_within_group */
    uint32_t group_addr = (pin_desc & 0xFFE0u) << 1u;     /* word-align to 32-pin block */
    uint16_t **gpio_blk = (uint16_t **)(group_addr + 0x7C00u); /* GPIO ctrl block ptr */
    uint32_t pin_bit = 1u << (pin_desc & 0x1Fu);           /* bitmask for pin */
    uint16_t *regs = *gpio_blk;                            /* resolved register base */

    EALLOW;

    /* bit2 = GPAAMSEL: analog mode select at offset +18 */
    if (flags & 0x04u) {
        regs[18] |=  (uint16_t)(pin_bit & 0xFFFFu);
        regs[19] |=  (uint16_t)(pin_bit >> 16u);
    } else {
        regs[18] &= ~(uint16_t)(pin_bit & 0xFFFFu);
        regs[19] &= ~(uint16_t)(pin_bit >> 16u);
    }

    /* bit0 = GPAPUD: pull-up disable at offset +12 */
    if (flags & 0x01u) {
        regs[12] |=  (uint16_t)(pin_bit & 0xFFFFu);
        regs[13] |=  (uint16_t)(pin_bit >> 16u);
    } else {
        regs[12] &= ~(uint16_t)(pin_bit & 0xFFFFu);
        regs[13] &= ~(uint16_t)(pin_bit >> 16u);
    }

    /* bit1 = GPAODR: open-drain at offset +16 (direct via gpio_blk ptr) */
    if (flags & 0x02u) {
        gpio_blk[16] |=  (uint16_t)(pin_bit & 0xFFFFu);
        gpio_blk[17] |=  (uint16_t)(pin_bit >> 16u);
    } else {
        gpio_blk[16] &= ~(uint16_t)(pin_bit & 0xFFFFu);
        gpio_blk[17] &= ~(uint16_t)(pin_bit >> 16u);
    }

    EDIS;
}


/* ===== gpio_set_mux  (OEM 0x082BA5, gpio) =====
 * EALLOW-protected GPIO multiplexer configuration. Given a pin descriptor in ACC (bits [15:8] = port-group byte, bits [1:0] = mux value 0..3) and AR6 = output storage, computes the GPIO ctrl block pointer from a lookup table and writes the 2-bit MUX field into both GPAMUX (offset 0) and GPAGMUX (offset 26) while preserving all other bits. Called by 44 sites during pin-function assignment.
 * Peripherals: GPIO ctrl 0x7C00; GPAMUX1/2 at offset 0 per port block; GPAGMUX1/2 at offset +26; pin group lookup table indexed by port_byte
 * [confidence: medium] */
/* 0x082BA5 - gpio_set_mux */
void gpio_set_mux(uint32_t pin_cfg, uint32_t *out_pin_cfg)
{
    /* pin_cfg: bits[15:8]=port_group_index, bits[1:0]=mux_value (0..3) */
    *out_pin_cfg = pin_cfg;  /* save original */

    EALLOW;

    /* derive GPIO ctrl block for this port group */
    uint8_t  port_byte = (uint8_t)((pin_cfg >> 8u) & 0xFFu);  /* sfr>>8, mask 0xFF */
    uint16_t **gpio_blk = (uint16_t **)(port_byte);            /* lookup table entry */
    uint16_t  mux_tbl_off = *(uint16_t *)gpio_blk;             /* AR2 = table->shift_pos */
    uint16_t  T = mux_tbl_off;                                 /* shift amount for 2-bit field */

    /* build clear mask: ~(3 << T) */
    uint32_t clr_mask = ~((uint32_t)3u << T);

    /* compute block base from pin_cfg upper part */
    uint32_t blk_base_idx = (pin_cfg >> 10u) & ~1u;
    uint16_t *regs = (uint16_t *)((blk_base_idx) + 0x7C00u);  /* GPIO ctrl block */

    /* clear old MUX field in GPAMUX (offset 0) */
    regs[0] &= (uint16_t)(clr_mask & 0xFFFFu);
    regs[1] &= (uint16_t)(clr_mask >> 16u);

    /* clear old MUX field in GPAGMUX (offset 26) */
    uint16_t *gmux = regs + 26u;
    gmux[0] &= (uint16_t)(clr_mask & 0xFFFFu);
    gmux[1] &= (uint16_t)(clr_mask >> 16u);

    /* compute new 2-bit field value from pin_cfg[1:0] */
    uint32_t mux_val = (pin_cfg & 3u) << T;

    /* write new GPAGMUX value */
    regs[26] |= (uint16_t)(mux_val & 0xFFFFu);
    regs[27] |= (uint16_t)(mux_val >> 16u);

    /* write new GPAMUX value */
    regs[0]  |= (uint16_t)(mux_val & 0xFFFFu);
    regs[1]  |= (uint16_t)(mux_val >> 16u);

    EDIS;
}


/* ===== gpio_ctrl_bitfield_write  (OEM 0x082D81, gpio) =====
 * Writes a bit-field (2-bit value from AL[1:0]) into a GPIO control register at a computed address. Derives the register word address as 31232 (0x7A00) plus the channel index, then reads the 32-bit register at XAR6=3 (a global config word), shifts a 2-bit mask to the pin position, inverts and ANDs to clear, then ORs the new value. EALLOW-protected. 3 callers.
 * Peripherals: GPIO or analog-IO control register at base 0x7A00 + channel_index (word address). Global config word at data address 3 (M0 area). EALLOW required.
 * [confidence: low] */
/* @0x082D81 - GPIO/analog-IO control register bit-field write
 * AL  = channel_index (0-based)
 * AH  = packed: bit[13]=bank_select, [7:0]=input_source or field_value
 * Called with EALLOW context.
 * TODO verify: 0x7A00 base + channel offset - exact peripheral unclear
 */
extern volatile uint32_t g_gpio_cfg_word; /* at data word address 3 */

void gpio_ctrl_bitfield_write(uint16_t channel_idx, uint16_t packed_ah)
{
    uint16_t bank     = (packed_ah >> 13) & 1u;  /* extract bit 13 */
    uint16_t reg_idx  = bank + channel_idx;       /* combine */
    uint16_t bit_pos  = (uint8_t)(packed_ah >> 8) & 0x1Fu; /* pin bit position */
    uint16_t field_val = packed_ah & 3u;          /* 2-bit value to write */
    uint16_t reg_addr = 0x7A00u + channel_idx;   /* computed peripheral address */
    volatile uint32_t *reg = (volatile uint32_t *)reg_addr;
    uint32_t mask;
    uint32_t val32;

    EALLOW;

    /* Read 32-bit config from address 3 */
    val32 = g_gpio_cfg_word; /* TODO verify: reads from word addr 3 */

    /* Build clear mask: invert 2-bit mask shifted to position */
    mask = ((uint32_t)3u << bit_pos);
    val32 &= ~mask;

    /* Build new field value shifted to position */
    val32 |= ((uint32_t)field_val << bit_pos);

    /* Write back to peripheral register at 0x7A00 + channel TODO verify */
    *reg = val32;

    EDIS;
}


/* ===== gpio_set_mux_4bit  (OEM 0x082E1C, gpio) =====
 * Sets a 4-bit GPIO mux field (2 bits from GPAMUX + 2 bits from GPAGMUX combined) for the pin number encoded in ACC. Computes the GPIO_CTRL register word address (base 0x7C00) from the pin group, clears the 4-bit field using mask 0xF, then ORs in the new mux value from AR4. EALLOW-protected. 45 callers (very common GPIO pin mux utility).
 * Peripherals: GPIO_CTRL block base 0x7C00 (EALLOW-protected): GPAMUX1/GPAMUX2/GPAGMUX1/GPAGMUX2 registers. Address computed as: ((pin & ~31) * 2 + 40) * 2 + 0x7C00. 4-bit mux field at pin*4 bit offset within 32-bit register.
 * [confidence: high] */
/* @0x082E1C - Set 4-bit GPIO mux field (GPAMUX + GPAGMUX combined) for one pin
 * ACC (XAR5) = packed pin descriptor: bits[7:5]=group, bits[4:0]=bit_position_in_reg
 * AR4 = 4-bit mux value to write (0-15)
 */
void gpio_set_mux_4bit(uint32_t pin_desc, uint16_t mux_val)
{
    uint32_t saved = pin_desc;
    uint16_t bit_pos;
    uint32_t reg_word_offset;
    volatile uint16_t *reg_lo;
    volatile uint16_t *reg_hi;
    uint32_t mask32, val32;

    /* Bit position within the 32-bit register = lower 5 bits */
    bit_pos = (uint16_t)(pin_desc >> 2) & 0x1Fu; /* from (pin_desc<<2)>>3 & 31 */

    /* Register group: clear lower 5 bits, divide by 8, add 20, double for word-addr, add GPIO base */
    reg_word_offset = (((saved & 0xFFE0u) + 20u) << 1u) + (31u << 10u); /* 31<<10 = 0x7C00 */
    reg_lo = (volatile uint16_t *)reg_word_offset;
    reg_hi = reg_lo + 1;

    /* Build 4-bit inverted mask and shift to position */
    mask32  = (uint32_t)0x0Fu << bit_pos;
    val32   = (uint32_t)(mux_val & 0x0Fu) << bit_pos;

    EALLOW;
    /* Clear then set the 4-bit field (spans two 16-bit words) */
    *reg_lo &= (uint16_t)(~mask32 & 0xFFFFu);
    *reg_hi &= (uint16_t)((~mask32 >> 16) & 0xFFFFu);
    *reg_lo |= (uint16_t)(val32 & 0xFFFFu);
    *reg_hi |= (uint16_t)((val32 >> 16) & 0xFFFFu);
    EDIS;
}


/* ===== gpio_output_write  (OEM 0x082E60, gpio) =====
 * Writes a single GPIO output pin high or low. Computes the GPIO data register word address from pin number (base 0x7C00, offset +20 words = 0x7C14 area). Creates a single-bit mask (1 << bit_pos). If AR4==1: ORs the mask in (set high). If AR4!=1: ANDs the inverted mask (set low). EALLOW-protected. 2 callers.
 * Peripherals: GPIO_CTRL block: register at 0x7C00 + (pin/32)*2*2 + 20 words (word-addressed). Single-bit mask derived from pin [4:0]. AR4=1 means set, AR4=0 means clear. EALLOW required.
 * [confidence: medium] */
/* @0x082E60 - Write a GPIO output pin high (dir=1) or low (dir=0)
 * ACC = pin_number (encoded: bits[4:0]=bit position within 32-bit group, rest=group)
 * AR4 = 1 to set high, 0 to set low
 */
void gpio_output_write(uint32_t pin_desc, uint16_t dir)
{
    uint16_t bit_pos  = (uint16_t)pin_desc & 0x1Fu;       /* lower 5 bits */
    uint32_t reg_base = ((pin_desc & 0xFFE0u) << 1u) + (31u << 10u); /* GPIO_CTRL group base */
    volatile uint16_t *reg_lo = (volatile uint16_t *)(reg_base + 20u);
    volatile uint16_t *reg_hi = reg_lo + 1;
    uint32_t bit_mask = (uint32_t)1u << bit_pos;

    EALLOW;
    if (dir == 1u)
    {
        /* Set output high */
        *reg_lo |= (uint16_t)(bit_mask & 0xFFFFu);
        *reg_hi |= (uint16_t)((bit_mask >> 16) & 0xFFFFu);
    }
    else
    {
        /* Set output low */
        *reg_lo &= (uint16_t)(~bit_mask & 0xFFFFu);
        *reg_hi &= (uint16_t)((~bit_mask >> 16) & 0xFFFFu);
    }
    EDIS;
}


/* ===== gpio_direction_set  (OEM 0x082EA2, gpio) =====
 * Sets or clears a GPIO pin direction bit in the GPADIR register (or equivalent at offset +10 from computed GPIO group base). Identical structure to gpio_output_write (sub_82E60) but uses offset +10 instead of +20 from the group base, targeting the direction register. If AR4==1: sets the bit (output); if AR4!=1: clears the bit (input). EALLOW-protected. 42 callers.
 * Peripherals: GPIO_CTRL block: GPADIR or GPBDIR register at 0x7C00 + group_offset + 10 words. Single-bit mask from pin [4:0]. EALLOW required.
 * [confidence: high] */
/* @0x082EA2 - Set GPIO pin direction (output=1, input=0)
 * ACC = pin_desc (encoded pin number: bits[4:0]=bit_pos, upper=group)
 * AR4 = 1 for output, 0 for input
 */
void gpio_direction_set(uint32_t pin_desc, uint16_t is_output)
{
    uint16_t bit_pos  = (uint16_t)pin_desc & 0x1Fu;
    uint32_t reg_base = ((pin_desc & 0xFFE0u) << 1u) + (31u << 10u); /* GPIO_CTRL group base */
    volatile uint16_t *dir_lo = (volatile uint16_t *)(reg_base + 10u); /* GPADIR lo */
    volatile uint16_t *dir_hi = dir_lo + 1;
    uint32_t bit_mask = (uint32_t)1u << bit_pos;

    EALLOW;
    if (is_output == 1u)
    {
        /* Configure as output: set direction bit */
        *dir_lo |= (uint16_t)(bit_mask & 0xFFFFu);
        *dir_hi |= (uint16_t)((bit_mask >> 16) & 0xFFFFu);
    }
    else
    {
        /* Configure as input: clear direction bit */
        *dir_lo &= (uint16_t)(~bit_mask & 0xFFFFu);
        *dir_hi &= (uint16_t)((~bit_mask >> 16) & 0xFFFFu);
    }
    EDIS;
}


/* ===== gpio_mux_write_csel2  (OEM 0x083001, gpio) =====
 * EALLOW-protected GPIO signal-mux write for the upper GPACSEL2 pair (offsets 24-25 of a GPIO control bank base in XAR6). Reads existing GPACSEL2[0] ([base+24]), masks with 0xC00F (preserves outer bits, clears middle field), inserts a new 5-bit value derived from (AR7-1) shifted left 5, ORs the result into the adjacent qualifying-select register at [XAR5-1], then writes back to [base+24] and writes 
 * Peripherals: GpioCtrlRegs: GPACSEL2 @ [GpioBase+24] and [GpioBase+25]. GPIO control base range 0x7C00-0x7FFF. EALLOW/EDIS required.
 * [confidence: low] */
/* 0x083001 - gpio_mux_write_csel2 */
void gpio_mux_write_csel2(volatile uint16_t *gpio_base /* XAR6 */,
                          uint16_t pin_in_bank    /* AR7 via stack arg_1 */,
                          uint16_t mux_val        /* AR4 */,
                          volatile uint16_t *qsel /* XAR5 */)
{
    EALLOW;
    uint16_t pin = pin_in_bank - 1u;
    uint16_t existing = gpio_base[24] & 0xC00Fu;  /* mask field */
    uint16_t insert   = (uint16_t)(pin << 5);      /* TODO verify shift */
    *qsel |= existing;                             /* or into adjacent reg */
    uint16_t merged = (uint16_t)(qsel[-1]) << 4;  /* TODO verify */
    merged |= existing;
    gpio_base[24] = merged;
    gpio_base[25] = mux_val;
    EDIS;
}


/* ===== gpio_mux_write_csel1  (OEM 0x083019, gpio) =====
 * EALLOW-protected GPIO signal-mux write for the lower GPACSEL1 pair (offsets 22-23 of a GPIO control bank base in XAR6). Identical structure to sub_83001 but targets GPACSEL1 ([base+22] and [base+23]). Used to configure the signal source for lower GPIO pins in a bank.
 * Peripherals: GpioCtrlRegs: GPACSEL1 @ [GpioBase+22] and [GpioBase+23]. GPIO control base range 0x7C00-0x7FFF. EALLOW/EDIS required.
 * [confidence: low] */
/* 0x083019 - gpio_mux_write_csel1 */
void gpio_mux_write_csel1(volatile uint16_t *gpio_base /* XAR6 */,
                          uint16_t pin_in_bank    /* AR7 via stack arg_1 */,
                          uint16_t mux_val        /* AR4 */,
                          volatile uint16_t *qsel /* XAR5 */)
{
    EALLOW;
    uint16_t pin = pin_in_bank - 1u;
    uint16_t existing = gpio_base[22] & 0xC00Fu;  /* mask field */
    uint16_t insert   = (uint16_t)(pin << 5);      /* TODO verify shift */
    *qsel |= existing;
    uint16_t merged = (uint16_t)(qsel[-1]) << 4;  /* TODO verify */
    merged |= existing;
    gpio_base[22] = merged;
    gpio_base[23] = mux_val;
    EDIS;
}
