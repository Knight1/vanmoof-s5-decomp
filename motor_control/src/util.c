/*
 * util.c -- VanMoof S5 motor_control (shared helpers + math).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== spin_forever  (OEM 0x082CE1, util) =====
 * Infinite busy-wait loop (NOP then unconditional branch to itself). Used as a CPU halt / fault sink. One caller (sub_82CE3).
 * Peripherals: None
 * [confidence: high] */
/* 0x082CE1 - spin_forever */
void spin_forever(void)
{
    for (;;) {
        __asm(" nop");
    }
}


/* ===== rom_api_call_wrapper  (OEM 0x0830A3, util) =====
 * Saves a minimal register context (ACC, DP, XAR0, XAR2, XAR3, XAR4, XAR5), resets the product-shift mode to 0, performs an indirect call through the function pointer stored at 0x70282 (a ROM API dispatch table entry or bootloader trampoline), then restores all saved registers and resets spm=0 before returning.
 * Peripherals: ROM/OTP function pointer at word address 0x70282. spm (product shift mode) reset to 0 around the call.
 * [confidence: medium] */
/* 0x0830A3 - rom_api_call_wrapper */
typedef void (*rom_fn_t)(void);
#define ROM_DISPATCH_ENTRY (*(rom_fn_t *)0x70282uL)

void rom_api_call_wrapper(void)
{
    /* saves ACC, DP, XAR0, XAR2, XAR3, XAR4, XAR5 on stack */
    __asm(" spm 0");
    ROM_DISPATCH_ENTRY(); /* indirect call via pointer at 0x70282 */
    __asm(" spm 0");
    /* restores registers from stack */
}


/* ===== check_array_min4_or_null  (OEM 0x0831AF, util) =====
 * Validates that the count in AL is at least 4. If AL >= 4 the pointer in XAR4 is returned unchanged; if AL < 4 XAR4 is zeroed (NULL) before returning. Used by the peripheral-init path (caller at 0x80732) to gate storage of a GSx-RAM descriptor pointer (0xC37C) only when the backing array holds enough elements.
 * Peripherals: GSx RAM 0xC37C (word_C34E descriptor slot); constant threshold 4
 * [confidence: high] */
/* 0x0831AF - check_array_min4_or_null */
/* Returns ptr if count >= 4, else NULL. AL=count, XAR4=ptr on entry. */
void *check_array_min4_or_null(uint16_t count, void *ptr)
{
    if (count < 4)
        ptr = NULL;
    return ptr;
}


/* ===== check_array_min16_or_null  (OEM 0x0831B7, util) =====
 * Validates that the count in AL is at least 16. If AL >= 16 the pointer in XAR4 is returned unchanged; if AL < 16 XAR4 is zeroed before returning. Called 4 times (0x8073D, 0x80745, 0x8074D, 0x80755) to gate four consecutive GSx-RAM descriptor pointer slots (0xC3C0, 0xC3D0, 0xC3E0, 0xC3F0) backing 16-element arrays.
 * Peripherals: GSx RAM descriptor slots 0xC3C0/0xC3D0/0xC3E0/0xC3F0; constant threshold 16
 * [confidence: high] */
/* 0x0831B7 - check_array_min16_or_null */
/* Returns ptr if count >= 16, else NULL. AL=count, XAR4=ptr on entry. */
void *check_array_min16_or_null(uint16_t count, void *ptr)
{
    if (count < 16)
        ptr = NULL;
    return ptr;
}


/* ===== check_array_min8_or_null  (OEM 0x0831BB, util) =====
 * Validates that the count in AL is at least 8. If AL >= 8 the pointer in XAR4 is returned unchanged; if AL < 8 XAR4 is zeroed before returning. Called once (0x807FB) to gate GSx-RAM descriptor slot 0xC390 backing an 8-element array.
 * Peripherals: GSx RAM descriptor slot 0xC390; constant threshold 8
 * [confidence: high] */
/* 0x0831BB - check_array_min8_or_null */
/* Returns ptr if count >= 8, else NULL. AL=count, XAR4=ptr on entry. */
void *check_array_min8_or_null(uint16_t count, void *ptr)
{
    if (count < 8)
        ptr = NULL;
    return ptr;
}


/* ===== check_array_min4b_or_null  (OEM 0x0831BF, util) =====
 * Identical semantics to check_array_min4_or_null (threshold 4). A separate instance called from a different site (0x807F3) to gate GSx-RAM descriptor slot 0xC380 (array base 50048). The compiler emitted a duplicate rather than sharing the prior instance, likely due to link-time placement constraints.
 * Peripherals: GSx RAM descriptor slot 0xC380; constant threshold 4
 * [confidence: high] */
/* 0x0831BF - check_array_min4b_or_null */
/* Returns ptr if count >= 4, else NULL. Duplicate of 0x831AF for a different array slot. */
void *check_array_min4b_or_null(uint16_t count, void *ptr)
{
    if (count < 4)
        ptr = NULL;
    return ptr;
}


/* ===== check_array_min10_or_null  (OEM 0x0831C3, util) =====
 * Validates that the count in AL is at least 10. If AL >= 10 the pointer in XAR4 is returned unchanged; if AL < 10 XAR4 is zeroed before returning. Called 4 times (0x8237F, 0x8260B, 0x8262C, 0x8264B) to gate GSx-RAM descriptor slots 0xC398/0xC3A2/0xC3AC backing 10-element arrays used by the FOC loop (likely CMPSS threshold or filter-coefficient arrays).
 * Peripherals: GSx RAM descriptor slots 0xC398/0xC3A2/0xC3AC; constant threshold 10
 * [confidence: high] */
/* 0x0831C3 - check_array_min10_or_null */
/* Returns ptr if count >= 10, else NULL. AL=count, XAR4=ptr on entry. */
void *check_array_min10_or_null(uint16_t count, void *ptr)
{
    if (count < 10)
        ptr = NULL;
    return ptr;
}


/* ===== clear_struct_fields6_7  (OEM 0x0831C7, util) =====
 * Zeroes words at offsets [6] and [7] of the struct pointed to by XAR4, then returns XAR4 unchanged. Called from the PIE/ISR vector-table initialisation (sub_824AE at 0x82520) after advancing XAR4 by 64 words. The two cleared words correspond to a pair of 16-bit fields (likely a count and a flag, or two sub-struct length fields) within a descriptor block in GSx RAM.
 * Peripherals: GSx RAM struct at XAR4+64; fields at word offsets 6 and 7
 * [confidence: high] */
/* 0x0831C7 - clear_struct_fields6_7 */
/* Clears words at offsets 6 and 7 in the struct pointed to by p. */
void clear_struct_fields6_7(uint16_t *p)
{
    p[6] = 0;
    p[7] = 0;
}


/* ===== store_acc32_to_struct_offset2  (OEM 0x0831D0, util) =====
 * Stores the 32-bit value in ACC to word offset [2] (i.e. bytes 4-7) of the struct pointed to by XAR4. Called from sub_83077 (0x83083) as the second of three consecutive stores that populate a small descriptor: [0] = source-field value, [2] = constant 29, [4] = constant 9.
 * Peripherals: GSx RAM struct target via XAR4; word offset 2
 * [confidence: medium] */
/* 0x0831D0 - store_acc32_to_struct_offset2 */
/* Stores 32-bit ACC into struct_ptr[2] (word offset 2). */
void store_acc32_to_struct_offset2(uint32_t value, uint32_t *p)
{
    p[1] = value;  /* word offset 2 = second 32-bit word (ACC is 32-bit) */
    /* Note: C28x 'movl *+XAR4[2],ACC' writes ACC as 32 bits to word addr XAR4+2 */
}


/* ===== store_acc32_to_struct_offset4  (OEM 0x0831D2, util) =====
 * Stores the 32-bit value in ACC to word offset [4] of the struct pointed to by XAR4. Called from sub_83077 (0x83088) as the third store in the same descriptor-population sequence (value = 9).
 * Peripherals: GSx RAM struct target via XAR4; word offset 4
 * [confidence: medium] */
/* 0x0831D2 - store_acc32_to_struct_offset4 */
/* Stores 32-bit ACC into struct_ptr[4] (word offset 4). */
void store_acc32_to_struct_offset4(uint32_t value, uint32_t *p)
{
    p[2] = value;  /* word offset 4 = third 32-bit word */
    /* Note: C28x 'movl *+XAR4[4],ACC' writes ACC as 32 bits to word addr XAR4+4 */
}


/* ===== store_acc32_to_struct_base  (OEM 0x0831D4, util) =====
 * Stores the 32-bit value in ACC to word offset [0] (base) of the struct pointed to by XAR4. Called from sub_83077 (0x8307E) as the first store in a descriptor-population sequence; the value comes from field [30] of the parent struct, and XAR4 comes from field [62] of the same parent struct.
 * Peripherals: GSx RAM struct target via XAR4; word offset 0 (base)
 * [confidence: high] */
/* 0x0831D4 - store_acc32_to_struct_base */
/* Stores 32-bit ACC into *struct_ptr (word offset 0). */
void store_acc32_to_struct_base(uint32_t value, uint32_t *p)
{
    p[0] = value;  /* word offset 0 = base of struct */
    /* C28x: 'movl *XAR4,ACC' = 32-bit store to [XAR4] */
}


/* ===== null_callback  (OEM 0x0831DC, util) =====
 * Empty no-op function (single lretr). Called at 0x82CB0 as the terminal callback in sub_82C8B's registered-hook dispatch loop, and also stored in the FLASH dispatch table at 0x8326E as a zero-count sentinel. Serves as a null/default handler for an unregistered slot.
 * Peripherals: None
 * [confidence: high] */
/* 0x0831DC - null_callback */
/* No-op: does nothing and returns. Used as a null/terminal callback. */
void null_callback(void)
{
    /* intentionally empty */
}
