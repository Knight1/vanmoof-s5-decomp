#ifndef USER_ECU_CRC8_H
#define USER_ECU_CRC8_H

/*
 * CRC-8, polynomial 0x31, computed over a 2-byte big-endian word.
 *
 * Quirk: the FIRST data byte is bitwise-inverted before the bit-banged
 * remainder loop (the OEM `mvns` on byte[0]); the SECOND byte is folded in
 * unmodified. There is no final XOR and no input/output reflection.
 */

#include <stdint.h>

/*
 * Bit-banged CRC-8 over the 2-byte word at *word.
 *   word[0] = high byte (inverted into the running CRC), word[1] = low byte.
 * Returns the 8-bit CRC in r0.
 */
uint8_t crc8_poly31_word(const uint8_t *word);

/*
 * Table-driven verifier for a small frame layout (see crc8.c). Returns 0 when
 * both embedded CRCs match, or 4 otherwise. On success it shifts two frame
 * bytes (frame[3] -> frame[2], frame[4] -> frame[3]).
 *
 * NOTE: depends on the OEM 256-byte lookup table `crc8_poly31_lut`, which lives
 * off-image (OEM @0x0001b364, past image end 0x0001a88b) and is therefore
 * declared extern here. See open_issues.
 */
uint32_t crc8_poly31_verify_lut(uint8_t *frame);

/* OEM CRC-8/poly-0x31 lookup table. OEM address 0x0001b364 (OFF-IMAGE). */
extern const uint8_t crc8_poly31_lut[256];

#endif /* USER_ECU_CRC8_H */
