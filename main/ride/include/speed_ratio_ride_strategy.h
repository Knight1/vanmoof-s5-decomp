/*
 * speed_ratio_ride_strategy.h - module model for the reconstructed VanMoof S5
 * i.MX8 `ride` service SpeedRatioRideStrategy (RideStrategy implementation).
 *
 * Included AFTER ride_common.h. Models the strategy object layout, the 14-byte
 * motor-command struct returned by ComputeCommand, and the off-image (.bss
 * runtime) per-region assist tables.
 *
 * Program "ride", AArch64, image base 0x100000. OEM addresses are quoted in the
 * .c file.
 */
#ifndef SPEED_RATIO_RIDE_STRATEGY_H
#define SPEED_RATIO_RIDE_STRATEGY_H

#include "ride_common.h"

/* ------------------------------------------------------------------------
 * SpeedRatioRideStrategy object (0x10-byte allocation).
 *   +0x00 : vptr (derived vtable DAT_0016ff80 / base vtable DAT_00170160)
 *   +0x08 : uint8_t assist_level   (0..4)  -- base-ctor arg, set via SetAssistLevel
 *   +0x09 : uint8_t region         (legal regime; picks the assist table)
 *   +0x0a : uint8_t boost          (boost flag; selects table entry index 5)
 * Modelled here as a plain struct; the .c casts the opaque `istrategy*`.
 * ------------------------------------------------------------------------ */
typedef struct speed_ratio_strategy {
    const void *vptr;     /* +0x00 */
    uint8_t assist_level; /* +0x08 */
    uint8_t region;       /* +0x09 */
    uint8_t boost;        /* +0x0a */
    uint8_t _pad[5];
} speed_ratio_strategy;

/* ------------------------------------------------------------------------
 * Motor command returned by ComputeCommand. The OEM returns a 16-byte
 * register pair (undefined1[16]) of which 14 bytes are meaningful: a type
 * byte at offset 0 followed by six little-endian u16 parameters. The high
 * 2 bytes (offset 14,15) are zeroed.
 *
 * GetFrames reads it as: type (byte0), and words w0..w5 at byte offsets
 * 2,4,6,8,10,12 -- i.e. it is the table entry copied verbatim.
 * ------------------------------------------------------------------------ */
typedef struct motor_cmd {
    uint8_t  type;       /* byte 0  -> SSP opcode select (1:0x1c, 2:0x1b, else:0x17) */
    uint8_t  _b1;        /* byte 1  (alignment, part of word region) */
    uint16_t w0;         /* bytes 2..3   */
    uint16_t w1;         /* bytes 4..5   */
    uint16_t w2;         /* bytes 6..7   */
    uint16_t w3;         /* bytes 8..9   */
    uint16_t w4;         /* bytes 10..11 */
    uint16_t w5;         /* bytes 12..13 */
} motor_cmd;

/* ------------------------------------------------------------------------
 * Per-region assist tables. These pointers live in .bss (DAT_00175528 etc.)
 * and are populated at runtime from a config blob; the numeric table values
 * are NOT in the image. Each table is an array of 14-byte entries:
 *   entry 0 = assist level 0  (offset 0x00)
 *   entry 1 = assist level 1  (offset 0x0e)
 *   entry 2 = assist level 2  (offset 0x1c)
 *   entry 3 = assist level 3  (offset 0x2a)
 *   entry 4 = assist level 4  (offset 0x38)
 *   entry 5 = boost           (offset 0x46)
 * Modelled as extern pointers to off-image data.
 * ------------------------------------------------------------------------ */
#define SPEED_RATIO_ENTRY_SIZE   0x0e   /* 14 bytes per assist-table entry */
#define SPEED_RATIO_BOOST_OFFSET 0x46   /* entry 5 (boost) byte offset      */

extern const uint8_t *speed_ratio_table_default; /* DAT_00175510 (region 0/other) */
extern const uint8_t *speed_ratio_table_region1; /* DAT_00175528 (region 1) */
extern const uint8_t *speed_ratio_table_region2; /* DAT_00175540 (region 2) */
extern const uint8_t *speed_ratio_table_region3; /* DAT_00175558 (region 3) */

/* SSP frame builders (ssp_protocol module). */
extern void ssp_frame_begin(ssp_buf *frame, int opcode, int seq);   /* 0x1224c0 */
extern void ssp_frame_append(ssp_buf *frame, uint16_t value);       /* 0x122460 */

/* std::vector<ssp_buf> model: a growable vector of frame byte-buffers. */
typedef struct ssp_frame_vec {
    ssp_buf *begin;
    ssp_buf *end;
    ssp_buf *cap;
} ssp_frame_vec;
void ssp_frame_vec_push(ssp_frame_vec *v, ssp_buf *frame); /* models FUN_00129b30 / inline grow */

#endif /* SPEED_RATIO_RIDE_STRATEGY_H */
