/*
 * crc8.c — CRC-8 (poly 0x31) over a 2-byte word.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 *
 * Functions:
 *   crc8_poly31_word        @ 0x0000955e  (bit-banged)
 *   crc8_poly31_verify_lut  @ 0x00006564  (table-driven, LUT off-image)
 */

#include "crc8.h"

/*
 * crc8_poly31_word — bit-banged CRC-8, polynomial 0x31, over a 2-byte word.
 * // 0x0000955e
 *
 * OEM logic (see disassembly 0x955e..0x959c):
 *   crc = ~word[0];                 // mvns: first byte inverted
 *   for (8 iterations) { MSB-first shift, conditional XOR 0x31 }
 *   crc ^= word[1];                 // second byte folded in unmodified
 *   for (8 iterations) { MSB-first shift, conditional XOR 0x31 }
 *   return (uint8_t)crc;            // uxtb r0; bx lr
 *
 * The OEM declares no explicit return type (Ghidra showed `void`), but the
 * final `uxtb r0` before `bx lr` and the caller frame_append_word_crc
 * (@0x000089b4), which stores the byte result, confirm a uint8_t return.
 */
uint8_t crc8_poly31_word(const uint8_t *word)
{
    uint8_t crc = (uint8_t)~word[0];

    for (uint_fast8_t i = 0; i < 8; i++) {
        uint8_t msb = crc & 0x80u;
        crc = (uint8_t)(crc << 1);
        if (msb != 0u) {
            crc ^= 0x31u;
        }
    }

    crc ^= word[1];

    for (uint_fast8_t i = 0; i < 8; i++) {
        uint8_t msb = crc & 0x80u;
        crc = (uint8_t)(crc << 1);
        if (msb != 0u) {
            crc ^= 0x31u;
        }
    }

    return crc;
}

/*
 * crc8_poly31_verify_lut — table-driven verify of a small frame. // 0x00006564
 *
 * Frame byte layout (offsets used by the OEM):
 *   frame[0], frame[1] : word A data bytes (hi, lo)
 *   frame[2]           : expected CRC of word A
 *   frame[3], frame[4] : word B data bytes (hi, lo)
 *   frame[5]           : expected CRC of word B
 *
 * Per-word CRC via the OEM LUT (functionally equivalent to the bit-banged
 * crc8_poly31_word): crc = lut[ lut[(uint8_t)~hi] ^ lo ].
 *
 * Returns 0 if both CRCs verify, otherwise 4. On success it shifts the frame:
 *   frame[2] = frame[3]; frame[3] = frame[4];
 * (OEM returns the status code in r0 as a 32-bit value.)
 */
uint32_t crc8_poly31_verify_lut(uint8_t *frame)
{
    uint8_t crc_a = crc8_poly31_lut[(uint8_t)(crc8_poly31_lut[(uint8_t)~frame[0]] ^ frame[1])];

    if (frame[2] == crc_a) {
        uint8_t crc_b = crc8_poly31_lut[(uint8_t)(crc8_poly31_lut[(uint8_t)~frame[3]] ^ frame[4])];

        if (frame[5] == crc_b) {
            frame[2] = frame[3];
            frame[3] = frame[4];
            return 0u;
        }
    }

    return 4u;
}
