#ifndef USER_ECU_I2C_H
#define USER_ECU_I2C_H

#include <stdint.h>

/*
 * VanMoof I2C transaction / opcode layer over the Ambiq IOM bus engine.
 *
 * Each builder fills a 28-byte (0x1c) transfer descriptor and hands it to the
 * vendor IOM transfer engine iom_i2c_transfer() together with a pointer to the
 * IOM context object (read from a global context-pointer cell in RAM).
 *
 * Opcodes (stored as a 16-bit field in the descriptor):
 *   0x53  / 0x153 : register write / register read
 *   0x44  / 0x144 : control-write / multi-byte read
 *   0x59  / 0x159 : byte-stream write / byte-stream read
 *
 * Status header 0xe28 is transmitted byte-swapped (little-endian bytes
 * {0x28, 0x0e}).
 */

/*
 * IOM transfer descriptor as filled by the builders below.
 *
 * Layout reconstructed from the OEM stack writes (descriptor base = sp+0x04):
 *   off 0x00 : flags / continuation word (always written 0 here)
 *   off 0x08 : 16-bit opcode field
 *   off 0x0c : word0  (peer 8-bit I2C address, or first stream byte)
 *   off 0x10 : word1  (always written 1 here)
 *   off 0x14 : word2  (register/data word, or data pointer)
 *   off 0x18 : word3  (extra data word, or byte count)
 *
 * The vendor engine reads it as int param_2[0..6]; we model it as a 7-word
 * array to preserve the exact offsets. The 0x1c-byte zero-fill (vmemset)
 * clears words [0..6]; the opcode halfword then overwrites the low half of
 * word[2] (off 0x08).
 */
typedef struct i2c_xfer_desc {
    uint32_t word[7]; /* 28 bytes (0x1c) */
} i2c_xfer_desc_t;

/*
 * vendor: Ambiq IOM I2C transfer engine - provided upstream, not reconstructed.
 * Returns 0 on success, or an OEM error code (e.g. 0xa28 / 0xa2b). Only the low
 * 16 bits of the return are used by the callers (sxth).
 */
extern int iom_i2c_transfer(int *iom_ctx, int *desc);

/* 0x00001a34 : register write, opcode 0x53. OEM tail-returns the
 * iom_i2c_transfer status in r0 (device_mgr_reset @0x3f14 checks it). */
int i2c_reg_write_53(uint32_t addr, uint32_t reg, uint32_t value);

/* 0x00001a70 : register read, opcode 0x153. */
void i2c_reg_read_153(uint32_t addr, uint32_t reg, uint32_t out);

/* 0x00007408 : byte-stream write, opcode 0x59. Returns iom_i2c_transfer status (sxth). */
int i2c_tx_frame(uint8_t *frame, int len);

/*
 * 0x000073a0 : byte-stream read, opcode 0x159. Reads (len/2)*3 bytes into buf as
 * {hi, lo, crc} groups, verifies each CRC-8, and compacts each group to {hi, lo}
 * in place. Returns the iom status (sxth), or 1 on a CRC mismatch.
 */
int i2c_rx_frame_verify(uint8_t *buf, unsigned int len);

/* 0x00007364 : control write, opcode 0x44. Returns the iom_i2c_transfer
 * status (0 on success); callers (e.g. sensor read) check it. */
int i2c_control_write(uint32_t value);

/*
 * 0x00008964 : read the 0xe28 status word. Transmits the byte-swapped header,
 * waits, reads back the response and stores the byte-swapped status to *out.
 */
void i2c_read_status_e28(uint16_t *out);

#endif /* USER_ECU_I2C_H */
