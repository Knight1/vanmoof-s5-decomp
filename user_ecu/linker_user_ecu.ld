/* linker_user_ecu.ld
 *
 * PROVISIONAL memory map for the VanMoof S5/A5 user_ecu (ARM Cortex-M4F,
 * VFPv4 hard-float; NXP S32K-like, exact SKU TBC).
 * Derived from the OEM image, NOT yet from a datasheet — treat the exact
 * flash origin and SRAM size as TBC until the MCU is confirmed (see
 * docs/hardware.md). What IS known from the image:
 *
 *   - The image is linked at base 0x00000000 (every vector-table target is a
 *     low 0x0000xxxx address; e.g. SVCall=0xf40, SysTick=0xa22e). On real
 *     silicon this is either true flash-at-0 or flash aliased to 0x0.
 *   - Image (code+rodata+data-init) spans 0x00000000 .. 0x0001a88b (108 684 B).
 *   - Initial SP = 0x20010000, so SRAM starts at 0x20000000 and is >= 64 KB.
 *   - A peripheral access to 0x40080000 was observed in a literal pool.
 *
 * Reset handling is non-standard: the vector table at 0x0 is a decoy, and the
 * real reset stub (Reset_Handler @0x1d4) relocates the runtime table to
 * VTOR=0xc00 before reaching main. The startup .S we eventually write must
 * replicate that (see docs/architecture.md). ENTRY(Reset_Handler) below is the
 * real entry name; its load placement is still TBC pending the live-table work.
 */

ENTRY(Reset_Handler)

MEMORY
{
    /* TBC: real origin may be 0x08000000 (STM32) aliased to 0x0; length is a
     * lower bound rounded up from the image size. */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);      /* OEM initial SP = 0x20010000 */
_Min_Stack_Size = 0x800;                    /* TBC */
_Min_Heap_Size  = 0x0;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text) *(.text*)
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
        KEEP (*(.init)) KEEP (*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata) *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data) *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    _image_end = LOADADDR(.data) + SIZEOF(.data);

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss) *(.bss*) *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    /DISCARD/ : { libc.a ( * ) libm.a ( * ) }
    .ARM.attributes 0 : { *(.ARM.attributes) }
}
