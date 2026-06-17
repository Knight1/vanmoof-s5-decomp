#ifndef USER_ECU_CONTROL_H
#define USER_ECU_CONTROL_H

#include <stdint.h>

#include "q16.h"

/*
 * control.h - VanMoof LED-ring control module.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M VFPv4 hard-float, FreeRTOS).
 *
 * The LED-ring control runs a per-tick Q16.16 easing/control kernel
 * (ledEasing_ControlUpdate) over a ~0xa0-byte state struct, once per channel.
 * The two channel structs live in SRAM:
 *   g_ctrl_struct_A @ 0x20001ce4
 *   g_ctrl_struct_B @ 0x20000860
 *
 * Field names below are best-effort reconstructions from the disassembly.
 * Offsets are verbatim (see ledParams_Init @0xb60 / ledEasing_ControlUpdate
 * @0xc64). Fields whose semantic meaning is uncertain keep an off_XX name with
 * a note in control.c's open issues.
 */

/*
 * LED-ring channel control state.
 *
 * All numeric fields are signed Q16.16 (1.0 == 0x00010000) unless noted.
 * Total size is 0xa0 bytes (40 words). q16_sigmoid() reads gain @+0x6c and
 * x0 @+0x70, which the kernel rewrites before each sigmoid evaluation.
 */
typedef struct LedCtrlState {
    int32_t  variant;        /* +0x00 channel variant selector (1 == variant A) */
    q16_t    level_base;     /* +0x04 base brightness/level (also used as hold value) */
    q16_t    cmd_step;       /* +0x08 command tracking accumulator (counts toward target) */
    q16_t    off_0c;         /* +0x0c limit threshold (compared to field +0x68) */
    q16_t    off_10;         /* +0x10 written into +0x70 (sigmoid x0) during ramp */
    q16_t    off_14;         /* +0x14 sigmoid target term A */
    q16_t    off_18;         /* +0x18 sigmoid target term B (rsb base 0x1fe0000) */
    q16_t    gain_scale;     /* +0x1c easing gain applied to the normalized delta */
    q16_t    off_20;         /* +0x20 */
    q16_t    off_24;         /* +0x24 */
    q16_t    off_28;         /* +0x28 */
    q16_t    tick;           /* +0x2c startup ramp tick (counts up to 0x2d0000) */
    q16_t    delta;          /* +0x30 (target - current) << 16 for this tick */
    q16_t    out_value;      /* +0x34 kernel output level (pre-quantization) */
    uint8_t  out_init;       /* +0x38 byte: momentum/output state initialized flag */
    uint8_t  pad_39[3];      /* +0x39 padding */
    q16_t    off_3c;         /* +0x3c momentum accumulator (paired w/ +0x40) */
    q16_t    off_40;         /* +0x40 momentum position */
    q16_t    off_44;         /* +0x44 sigmoid-input seed / state (init 0x320000) */
    q16_t    off_48;         /* +0x48 q16_div(0x2469, 0xc0012) */
    q16_t    off_4c;         /* +0x4c q16_div(0x48d,  0xc0012) */
    int32_t  off_50;         /* +0x50 pointer/handle (variant-dependent literal) */
    q16_t    off_54;         /* +0x54 0x68d */
    q16_t    off_58;         /* +0x58 */
    q16_t    off_5c;         /* +0x5c */
    q16_t    off_60;         /* +0x60 sigmoid-input ramp A (counts up) */
    q16_t    off_64;         /* +0x64 sigmoid-input ramp B (counts up) */
    q16_t    off_68;         /* +0x68 limit accumulator */
    q16_t    sig_gain;       /* +0x6c sigmoid gain  (read by q16_sigmoid @+0x6c) */
    q16_t    sig_x0;         /* +0x70 sigmoid offset (read by q16_sigmoid @+0x70) */
    q16_t    off_74;         /* +0x74 0x320000 */
    q16_t    off_78;         /* +0x78 */
    q16_t    off_7c;         /* +0x7c easing time/denominator term */
    q16_t    off_80;         /* +0x80 easing offset term */
    q16_t    off_84;         /* +0x84 mode select (==0x10000 selects branch) */
    q16_t    off_88;         /* +0x88 smoothing coeff A */
    q16_t    off_8c;         /* +0x8c smoothing coeff B */
    uint8_t  smooth_init;    /* +0x90 byte: smoothing state initialized flag */
    uint8_t  pad_91[3];      /* +0x91 padding */
    q16_t    smooth_a;       /* +0x94 smoothing state A */
    q16_t    smooth_b;       /* +0x98 smoothing state B */
    q16_t    smooth_c;       /* +0x9c smoothing state C */
} LedCtrlState;

/*
 * Initialise an LED-ring channel state struct. // 0x00000b60
 *
 * 'variant' selects one of two field presets (variant == 1 vs. otherwise).
 * params_a/params_b are forwarded to q16_div for the +0x48 init value
 * (in the OEM both are the same magic constant pair).
 */
void ledParams_Init(LedCtrlState *state, int variant, q16_t params_a, q16_t params_b);

/*
 * Per-tick Q16.16 easing/control kernel. // 0x00000c64
 *
 * Advances 'state' by one tick toward 'target' (a scaled sensor count) and
 * writes the quantized output level (16.16 >> 16, signed) to *out.
 */
void ledEasing_ControlUpdate(LedCtrlState *state, int target, int *out);

/*
 * controlTask command handler. // 0x00001ee0
 *
 * cmd 0x1c / 0x1d: read scaled sensor counts (app header tag 0x1926) over the
 *                  i2c layer, run the kernel on both channels A and B, and
 *                  return one channel's quantized output in result[0].
 * cmd 0x3a:        read a status word (i2c sub-command 0xe28) into result[0].
 * any other cmd:   returns 4.
 *
 * On success result[0] holds the value, result[1] is cleared, returns 0;
 * otherwise returns the propagated error code.
 */
int controlTask_CmdHandler(int cmd, uint32_t *result, uint32_t arg, uint32_t ctx);

/*
 * int_pair_to_float — convert a {whole, micros} signed int pair to float:
 * p[0] + p[1] / 1e6 (single precision). The first word is the whole part, the
 * second counts millionths. Used by the control task. // 0x00000884
 */
float int_pair_to_float(const int32_t *p);

#endif /* USER_ECU_CONTROL_H */
