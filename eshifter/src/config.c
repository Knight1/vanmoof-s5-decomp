/*
 * config.c — eshifter calibration-config persistence + VMFW header emit.
 *
 * VanMoof S5/A5 electronic gear-shifter controller. Translated from the OEM
 * image (NXP LPC546xx, base 0x0). The NVM/OD register-write driver is vendor;
 * the XOR-checksummed config layout and the VMFW framing are VanMoof. RAM struct
 * fields are accessed as volatile literal-offset stores to match the OEM.
 */

#include "eshifter.h"

/* eshifter_write_config_record — checksum + persist the 0x80-byte config record.
 *
 * OEM disassembly (0x000068c0..0x000068fa):
 *
 * Computes a 16-bit XOR over the config payload halfwords from record+0x0a up to
 * (not including) record+0x84 (seed = the halfword at +0x0a), stores the result
 * at record+0x86, then streams the 0x80-byte payload (base record+0x0a) out in
 * eight 0x10-byte chunks via the OD register-write driver (register 0x50), with
 * the running byte offset (truncated to 8 bits) as the index and the two trailing
 * context words forwarded unchanged.
 */
void eshifter_write_config_record(uint8_t *record)
{
    volatile uint16_t *p = (volatile uint16_t *)(record + 0x0a);
    volatile uint16_t *q = p;
    uint16_t xsum = *p;
    uint32_t off;

    do {
        q = q + 1;
        xsum ^= *q;
    } while (q != (volatile uint16_t *)(record + 0x84));

    *(volatile uint16_t *)(record + 0x86) = xsum;

    off = 0;
    do {
        uint8_t *chunk = (uint8_t *)p + off;
        uint32_t idx = off & 0xff;
        off += 0x10;
        nvm_record_block_write_verify(record, 0x50, idx, chunk, 0x10);
    } while (off != 0x80);
}

/* eshifter_emit_vmfw_header — build a VanMoof VMFW framing header and send it.
 *
 * OEM disassembly (0x00006fd0..0x0000700c):
 *
 * Clears the ctx err/status byte (+8), lays down the 6-byte header
 * {'V','M','F','W', 0x02, 0x00} at buf[0..5], pads the remaining 0xf8 bytes to
 * 0xff (total framed size 0xfe), and sends it via eshifter_od_signal_send_51(ctx,
 * 0, buf, 0xfe). If the send reports a non-zero (failure) status it sets the ctx
 * err flag at +9.
 */
void eshifter_emit_vmfw_header(void *ctxv, void *bufv)
{
    uint8_t *ctx = (uint8_t *)ctxv;
    uint8_t *buf = (uint8_t *)bufv;

    *(volatile uint8_t *)(ctx + 8) = 0;

    buf[0] = 0x56; /* 'V' */
    buf[1] = 0x4d; /* 'M' */
    buf[2] = 0x46; /* 'F' */
    buf[3] = 0x57; /* 'W' */
    buf[4] = 0x02; /* VMFW format/version 2 */
    buf[5] = 0x00;

    mem_set(buf + 6, 0xff, 0xf8);

    if (eshifter_od_signal_send_51(ctx, 0, (uint16_t *)buf, 0xfe) != 0)
        *(volatile uint8_t *)(ctx + 9) = 1;
}
