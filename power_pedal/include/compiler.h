#ifndef POWER_PEDAL_COMPILER_H
#define POWER_PEDAL_COMPILER_H

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

#endif /* POWER_PEDAL_COMPILER_H */
