/*
 * adc.c — VanMoof power_control ADC / thermal sensing.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin   (image base 0x0)
 *
 * Functions:
 *   adc_to_temperature_lookup   @ 0x00000e9c
 */

#include "power_control.h"

/*
 * adc_to_temperature_lookup — NTC/thermistor ADC->temperature lookup. // 0x00000e9c
 *
 * OEM disassembly (0x00000e9c..0x00000ebb):
 *
 * Loads the most recent ADC sample (a uint16 at POWER_ADC_STATE+4) and walks the
 * 166-entry monotonically-decreasing uint16 calibration table g_ntc_temp_table.
 * It advances an index until a table entry drops below the sample; the
 * temperature is then the index biased by -0x28 (so table[0] => -40 degC),
 * sign-extended to a signed char. If no entry is below the sample (colder than
 * the whole table) it returns 0x7f as an out-of-range sentinel.
 */
int adc_to_temperature_lookup(void)
{
    const uint16_t *p = g_ntc_temp_table;
    uint16_t sample = *(volatile uint16_t *)(POWER_ADC_STATE + 4u);
    int i;

    for (i = 0; i != 0xa6; i++) {
        if (sample > *p) {
            return (signed char)((char)i - 0x28);
        }
        p++;
    }
    return 0x7f;
}
