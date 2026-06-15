/*
 * i2c.c - VanMoof I2C transaction / opcode layer over the Ambiq IOM bus engine.
 *
 * Translated from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin.
 * Targets ARM Cortex-M4F (ARMv7-M, VFPv4 hard-float), FreeRTOS.
 *
 * Each builder zero-fills a 28-byte descriptor, writes the opcode + parameter
 * words at the exact OEM offsets, then calls the vendor IOM transfer engine
 * (iom_i2c_transfer @0x7288) with the IOM context pointer read from a global
 * context-pointer cell in RAM.
 */

#include <stdint.h>
#include <stddef.h>

#include "i2c.h"
#include "crc8.h"
#include "util.h"   /* vmem_set */

/*
 * IOM context pointers in RAM. Each builder loads its pointer cell and
 * dereferences it once to obtain the IOM context object passed to
 * iom_i2c_transfer (e.g. `r7 = *(void**)0x20000808`).
 *
 *   DAT_00001a6c / DAT_00001a9c -> 0x20000808 (reg write / reg read)
 *   DAT_00007448 / DAT_00007404 -> 0x200007f4 (stream tx / stream rx)
 *   DAT_0000739c                -> 0x200007f8 (control write)
 *
 * These live in vendor RAM and are populated by the IOM driver init, so they
 * are declared extern (not reconstructed here).
 */
/* vendor: IOM context pointer cells - provided upstream, not reconstructed */
extern int *g_iom_ctx_regio;  /* @0x20000808 */
extern int *g_iom_ctx_stream; /* @0x200007f4 */
extern int *g_iom_ctx_ctrl;   /* @0x200007f8 */

/*
 * vendor: RTOS millisecond delay / yield (FUN_00001bdc) - provided upstream,
 * not reconstructed. Used to wait between TX and RX of a status read.
 */
extern void rtos_delay_00001bdc(uint32_t ms);

/* 0x00001a34 */
void i2c_reg_write_53(uint32_t addr, uint32_t reg, uint32_t value)
{
    int *ctx = g_iom_ctx_regio;            /* *DAT_00001a6c */
    i2c_xfer_desc_t desc;

    vmem_set(&desc, 0, 0x1c);
    desc.word[0] = 0;                      /* flags          */
    desc.word[2] = 0x53;                   /* opcode (u16)   */
    desc.word[3] = addr;                   /* peer I2C addr  */
    desc.word[4] = 1;
    desc.word[5] = reg;
    desc.word[6] = value;
    iom_i2c_transfer(ctx, (int *)&desc);
}

/* 0x00001a70 */
void i2c_reg_read_153(uint32_t addr, uint32_t reg, uint32_t out)
{
    /* OEM does NOT zero-fill this descriptor; word[1] is left untouched. We
     * zero-initialise to keep the C well-defined (see open_issues). */
    i2c_xfer_desc_t desc = { { 0, 0, 0, 0, 0, 0, 0 } };

    desc.word[2] = 0x153;                  /* opcode (u16)   */
    desc.word[0] = 0;
    desc.word[3] = addr;
    desc.word[4] = 1;
    desc.word[5] = reg;
    desc.word[6] = out;
    iom_i2c_transfer(g_iom_ctx_regio, (int *)&desc); /* *DAT_00001a9c */
}

/* 0x00007408 */
int i2c_tx_frame(uint8_t *frame, int len)
{
    uint8_t first = *frame;                /* ldrb r7,[r5],#1 */
    int *ctx = g_iom_ctx_stream;           /* *DAT_00007448 */
    i2c_xfer_desc_t desc;
    int16_t status;

    vmem_set(&desc, 0, 0x1c);
    desc.word[2] = 0x59;                   /* opcode (u16)   */
    desc.word[4] = 1;
    desc.word[5] = (uint32_t)(uintptr_t)(frame + 1);
    desc.word[6] = (uint32_t)((len - 1) & 0xff); /* count = len-1 (byte) */
    desc.word[0] = 0;
    desc.word[3] = first;                  /* first stream byte */
    status = (int16_t)iom_i2c_transfer(ctx, (int *)&desc); /* sxth */
    return status;
}

/* 0x000073a0 */
int i2c_rx_frame_verify(uint8_t *buf, unsigned int len)
{
    i2c_xfer_desc_t desc;
    unsigned int count;
    int16_t status;
    uint8_t *p;
    uint8_t *out;

    desc.word[2] = 0x159;                  /* opcode (u16)   */
    desc.word[0] = 0;
    desc.word[3] = 0;
    desc.word[4] = 0;
    /* count = (len >> 1) * 3, truncated to 16 bits */
    count = ((len >> 1) * 3) & 0xffff;
    desc.word[5] = (uint32_t)(uintptr_t)buf;
    desc.word[6] = count;
    status = (int16_t)iom_i2c_transfer(g_iom_ctx_stream, (int *)&desc); /* *DAT_00007404 */

    if (status == 0) {
        p = buf;
        out = buf;
        do {
            uint8_t crc = crc8_poly31_word(p); /* FUN_0000955e, r1=p preserved */
            if ((unsigned int)p[2] != (unsigned int)crc) {
                return 1;                  /* CRC mismatch */
            }
            out[0] = p[0];
            out[1] = p[1];
            p += 3;
            out += 2;
        } while ((unsigned int)(((int)(p - buf)) & 0xffff) < count);
    }
    return (int)status;
}

/* 0x00007364 — OEM tail-returns the iom_i2c_transfer status in r0 (callers
 * such as sensor_read_sht_temp_humidity check it), so the return type is int. */
int i2c_control_write(uint32_t value)
{
    int *ctx = g_iom_ctx_ctrl;             /* *DAT_0000739c */
    i2c_xfer_desc_t desc;

    vmem_set(&desc, 0, 0x1c);
    desc.word[0] = 0;
    desc.word[1] = 0;
    desc.word[2] = 0x44;                   /* opcode (u16)   */
    desc.word[3] = value;
    desc.word[4] = 1;
    desc.word[5] = 0;
    desc.word[6] = 0;
    return iom_i2c_transfer(ctx, (int *)&desc);
}

/* 0x00008964 */
void i2c_read_status_e28(uint16_t *out)
{
    /* Header 0xe28 transmitted byte-swapped: bytes {0x28, 0x0e}.
     * The OEM reuses a 4-byte stack slot (push {r0,r1}, fields at sp+0x4..0x7):
     * i2c_rx_frame_verify(.,2) writes a 3-byte {hi,lo,crc} group into it before
     * compacting to 2 bytes, so the buffer must hold at least 3 bytes. */
    uint8_t frame[4] = { 0 };

    frame[0] = 0x28;
    frame[1] = 0x0e;
    if (i2c_tx_frame(frame, 2) == 0) {
        rtos_delay_00001bdc(0x140);
        if (i2c_rx_frame_verify(frame, 2) == 0) {
            uint16_t v = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
            /* rev16: swap the two bytes back into host order */
            *out = (uint16_t)((v << 8) | (v >> 8));
        }
    }
}
