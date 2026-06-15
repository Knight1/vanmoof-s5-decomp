#ifndef USER_ECU_SENSOR_H
#define USER_ECU_SENSOR_H

#include <stdint.h>

/*
 * Sensor module.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARMv7-M, VFPv4 hard-float, FreeRTOS).
 */

/*
 * Read a Sensirion SHT/SHTC-style temperature + relative-humidity sensor over
 * I2C and apply the OEM fixed-point conversion.  // 0x00001e1c
 *
 * channel selects WHICH quantity to convert from the sensor reply:
 *   channel == 0x0d  -> temperature
 *   channel == 0x10  -> relative humidity
 *   anything else    -> returns 4 (invalid channel), out[] untouched.
 *
 * The 7-bit-ish device sub-address byte is chosen at run time from the global
 * sensor-variant selector (*g_sensor_variant): 1 -> 0xf6, 2 -> 0xe0, else 0xfd.
 * A command write (i2c_control_write) primes the device, then a single IOM I2C
 * transfer (opcode word 0x50454741, command 0x144, 6-byte read) fetches the
 * reply.  The 6-byte reply holds two big-endian 16-bit samples, each followed
 * by a CRC-8 byte; crc8_poly31_verify_lut() validates both before conversion.
 *
 * Conversion (verbatim from the disassembly at 0x00001e8e / 0x00001ebc):
 *   raw16 = byteswap of the relevant reply halfword (zero-extended to 16 bit)
 *   temperature: scaled = ((raw16 * 0x5573) >> 13) - 45000
 *   humidity:    scaled = ((raw16 * 0x3d09) >> 13) -  6000
 *
 * 'scaled' is in milli-units (milli-degC for temperature, milli-%RH for
 * humidity).  The OEM splits it across two output words:
 *   out[0] = scaled / 1000            -> integer part (whole degC / whole %RH)
 *   out[1] = (scaled % 1000) * 1000   -> fractional part in micro-units
 *                                        (signed, same sign as out[0])
 *
 * Returns 0 on success; a non-zero error code from the I2C command write, the
 * IOM transfer, or the CRC verifier on failure; 4 for an invalid channel.
 */
int sensor_read_sht_temp_humidity(int channel, int *out);

#endif /* USER_ECU_SENSOR_H */
