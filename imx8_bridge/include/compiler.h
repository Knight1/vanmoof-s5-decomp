#ifndef IMX8_BRIDGE_COMPILER_H
#define IMX8_BRIDGE_COMPILER_H

/* Cross-compiler attribute helpers. We support GCC (arm-none-eabi) only. */

#define ALWAYS_INLINE   static inline __attribute__((always_inline))
#define NAKED           __attribute__((naked))
#define USED            __attribute__((used))
#define UNUSED          __attribute__((unused))
#define WEAK            __attribute__((weak))
#define ALIASED(name)   __attribute__((alias(#name)))
#define SECTION(s)      __attribute__((section(s)))
#define PACKED          __attribute__((packed))
#define ALIGNED(n)      __attribute__((aligned(n)))
#define NORETURN        __attribute__((noreturn))

/* Cortex-M barriers via inline asm (no CMSIS; zero deps). */
ALWAYS_INLINE void __nop(void)     { __asm volatile ("nop"); }
ALWAYS_INLINE void __dsb(void)     { __asm volatile ("dsb 0xF" ::: "memory"); }
ALWAYS_INLINE void __isb(void)     { __asm volatile ("isb 0xF" ::: "memory"); }
ALWAYS_INLINE void __wfi(void)     { __asm volatile ("wfi"); }
ALWAYS_INLINE void __cpsid_i(void) { __asm volatile ("cpsid i" ::: "memory"); }
ALWAYS_INLINE void __cpsie_i(void) { __asm volatile ("cpsie i" ::: "memory"); }

/* Current exception number (0 = thread mode). The OEM branches task-vs-ISR on
 * a bare `mrs rN, ipsr` (the decompiler shows phantom helper calls). */
ALWAYS_INLINE unsigned __get_ipsr(void)
{ unsigned r; __asm volatile ("mrs %0, ipsr" : "=r" (r)); return r; }

/* Raw 32-bit MMIO accessors (verbatim volatile loads/stores, no abstraction). */
#define MMIO32(addr) (*(volatile unsigned *)(addr))

#endif /* IMX8_BRIDGE_COMPILER_H */
