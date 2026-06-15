/*
 * sensor.c - SHT/SHTC temperature + humidity sensor read.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS). Behaviour-faithful translation of
 * sensor_read_sht_temp_humidity @ 0x00001e1c.
 */

#include <stdint.h>

#include "sensor.h"
#include "crc8.h" /* crc8_poly31_verify_lut */

/*
 * I2C command-write helper (i2c_control_write @ 0x00007364), part of the i2c
 * layer. NOTE: include/i2c.h currently prototypes this as returning void, but
 * the OEM body tail-returns the iom_i2c_transfer status in r0 and the caller
 * here checks it (bl 0x00007364; mov r5,r0; cmp r0,#0). We therefore declare
 * the true OEM ABI locally (int) and intentionally do NOT #include "i2c.h",
 * which would clash on this prototype. See open_issues.
 */
extern int i2c_control_write(uint32_t value);

/*
 * vendor: Ambiq IOM I2C transfer engine (iom_i2c_transfer @ 0x00007288) -
 * provided upstream, not reconstructed. First arg is the IOM handle, second
 * arg points at a 7-word transfer descriptor (see sht_iom_descriptor_t).
 */
extern int iom_i2c_transfer(int *iom_handle, int *descriptor);

/*
 * vendor: I2C "set up next command / wake" helper (FUN_00001bdc @ 0x00001bdc) -
 * provided upstream, not reconstructed. Called with 0xf before the read.
 */
extern void FUN_00001bdc(uint32_t arg);

/*
 * Run-time globals referenced via the literal pool at 0x00001ed4 / 0x00001edc.
 * These live in RAM (uninitialised in the image) and are owned by the I2C /
 * board layer, so they are declared extern here rather than reconstructed.
 *
 *   *DAT_00001ed4 -> byte at RAM 0x200070df: sensor-variant selector.
 *   *DAT_00001edc -> word at RAM 0x200007f8: pointer to the Ambiq IOM handle.
 */
extern volatile uint8_t g_sensor_variant; /* OEM RAM 0x200070df (DAT_00001ed4) */
extern int *g_iom_handle;                 /* OEM RAM 0x200007f8 (DAT_00001edc) */

/*
 * IOM transfer descriptor layout consumed by iom_i2c_transfer.
 * Field order mirrors the words the engine reads as descriptor[0..6]; the
 * command field is written as a 16-bit value by the OEM (strh) but occupies a
 * full descriptor word.
 */
typedef struct {
    int      status;   /* descriptor[0] : seeded with the command-write result */
    uint32_t opcode;   /* descriptor[1] : DAT_00001ed8 == 0x50454741           */
    uint16_t command;  /* descriptor[2] : 0x144                                */
    uint16_t _pad;     /* (high half of descriptor[2], left untouched)         */
    uint32_t devaddr;  /* descriptor[3] : device sub-address byte (0xf6/e0/fd) */
    int      tx_len;   /* descriptor[4] : 1                                    */
    uint16_t *rx_buf;  /* descriptor[5] : pointer to the 6-byte reply buffer   */
    int      rx_len;   /* descriptor[6] : 6                                    */
} sht_iom_descriptor_t;

int sensor_read_sht_temp_humidity(int channel, int *out) /* 0x00001e1c */
{
    int rc;
    int scaled;
    uint32_t devaddr;
    /* 6-byte reply: two BE16 samples each followed by a CRC-8 byte. */
    uint16_t reply[3];

    if ((channel != 0xd) && (channel != 0x10)) {
        return 4; /* 0x00001ed0: invalid channel */
    }

    /* 0x00001e2c: pick the device sub-address byte by sensor variant. */
    if (g_sensor_variant == 1) {
        devaddr = 0xf6; /* 0x00001eb8 */
    } else if (g_sensor_variant == 2) {
        devaddr = 0xe0; /* 0x00001e3a */
    } else {
        devaddr = 0xfd; /* 0x00001e38 */
    }

    /* 0x00001e3e: prime the device with the command write. */
    rc = i2c_control_write(devaddr);
    if (rc != 0) {
        return rc; /* 0x00001eb2 */
    }

    /* 0x00001e48: vendor I2C wake/setup before the read. */
    FUN_00001bdc(0xf);

    /* 0x00001e4e..0x00001e6e: build the 7-word transfer descriptor. */
    sht_iom_descriptor_t desc;
    desc.status  = rc;          /* 0 here (the command-write result) */
    desc.opcode  = 0x50454741u; /* DAT_00001ed8 (literal pool @ 0x00001ed8) */
    desc.command = 0x144;
    desc.devaddr = devaddr;
    desc.tx_len  = 1;
    desc.rx_buf  = reply;
    desc.rx_len  = 6;

    /* 0x00001e70: issue the transfer. */
    rc = iom_i2c_transfer(g_iom_handle, (int *)&desc);
    if (rc != 0) {
        return rc; /* 0x00001eb2 */
    }

    /* 0x00001e7a: validate both embedded CRC-8 bytes. */
    rc = crc8_poly31_verify_lut((uint8_t *)reply);
    if (rc != 0) {
        return rc; /* 0x00001eb2 */
    }

    /*
     * 0x00001e82: convert the selected sample. Each path byte-swaps the
     * relevant BE16 halfword, zero-extends it, multiplies by the sensor
     * coefficient, arithmetic-shifts right by 13, then applies the offset.
     */
    if (channel == 0xd) {
        /* temperature: reply[0] */
        uint32_t raw = (uint32_t)(uint16_t)((reply[0] << 8) | (reply[0] >> 8));
        scaled = (int)(raw * 0x5573u) >> 0xd; /* 0x00001e90 / 0x00001e98 */
        scaled -= 0xaf00;                      /* 0x00001e9a: -44800 */
        scaled -= 0xc8;                        /* 0x00001e9e: -200  (=> -45000) */
    } else {
        /* humidity: reply[1] */
        uint32_t raw = (uint32_t)(uint16_t)((reply[1] << 8) | (reply[1] >> 8));
        scaled = (int)(raw * 0x3d09u) >> 0xd; /* 0x00001ebc / 0x00001ec6 */
        scaled -= 0x1760;                      /* 0x00001ec8: -5984 */
        scaled -= 0x10;                        /* 0x00001ecc: -16   (=> -6000) */
    }

    /* 0x00001ea0..0x00001eb0: split into integer and micro-unit parts. */
    out[0] = scaled / 1000;
    out[1] = (scaled % 1000) * 1000;

    return rc; /* 0 */
}
