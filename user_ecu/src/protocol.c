#include "protocol.h"
#include "crc8.h"

#include <stdint.h>

/*
 * frame_append_word_crc — append a big-endian 16-bit word and its CRC-8 byte.
 * // 0x0000899c
 *
 * Faithful translation of the OEM routine. The buffer base and offset are
 * passed as integers (the OEM treats the base as an int and forms byte
 * pointers by addition). The offset increments are masked to 16 bits, matching
 * the uxth instructions in the original.
 *
 *   buf[off]   = (word >> 8) & 0xff;   high byte
 *   buf[off+1] = word & 0xff;          low  byte
 *   buf[off+2] = crc8_poly31_word(&buf[off]);   CRC over the two word bytes
 *
 * Returns (off + 3) & 0xffff.
 */
uint32_t frame_append_word_crc(int32_t buf, int32_t off, uint32_t word)
{
	uint8_t crc;
	uint32_t crc_off;

	/* High byte at buf[off]. */
	*(char *)(buf + off) = (char)(word >> 8);

	/* Offset of the CRC byte, kept in 16-bit modular space. */
	crc_off = (uint32_t)(off + 2) & 0xffffU;

	/* Low byte at buf[(off + 1) & 0xffff]. */
	*(char *)(buf + (int32_t)((uint32_t)(off + 1) & 0xffffU)) = (char)word;

	/* CRC-8 over the two bytes just written, i.e. &buf[off]. */
	crc = crc8_poly31_word((const uint8_t *)(buf + (int32_t)(crc_off - 2)));

	*(uint8_t *)(buf + (int32_t)crc_off) = crc;

	return (uint32_t)(off + 3) & 0xffffU;
}
