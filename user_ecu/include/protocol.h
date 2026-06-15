#ifndef USER_ECU_PROTOCOL_H
#define USER_ECU_PROTOCOL_H

#include <stdint.h>

/*
 * Frame builder helpers for the user_ecu TX protocol.
 *
 * frame_append_word_crc() writes a big-endian 16-bit word followed by a
 * CRC-8 byte (polynomial 0x31, computed over the two word bytes) into a TX
 * buffer and returns the new offset.
 */

/*
 * Append a big-endian 16-bit word plus its CRC-8 to buf.
 *
 *   buf[off]   = word >> 8;          (high byte)
 *   buf[off+1] = word & 0xff;        (low  byte)
 *   buf[off+2] = crc8_poly31_word(&buf[off]);
 *
 * The offset arithmetic is performed in 16-bit (uint16) modular space, exactly
 * as the OEM does. Returns (off + 3) & 0xffff.
 *
 * Matches the OEM ABI: buffer base and offset are passed as ints, the word as
 * a 32-bit value (only the low 16 bits are used).
 */
uint32_t frame_append_word_crc(int32_t buf, int32_t off, uint32_t word);

#endif /* USER_ECU_PROTOCOL_H */
