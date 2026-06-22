/*
 * report.c — charger CAN telemetry report builder.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). Builds and transmits four
 * telemetry frames on the charger OD node (0x14E2xxxx): the scaled current
 * setpoint + voltage + packed setpoint limits, a 2-byte value, the charge mode,
 * and a 6-byte packed status word. Translated from the OEM image (raw ARM
 * Cortex-M, base 0x0). The measurement reads and the M_CAN Tx enqueue are vendor;
 * the frame layout/scaling is the charger app.
 */

#include "charger.h"

/* charger_build_can_reports — assemble + transmit the charger telemetry frames.
 *
 * OEM disassembly (0x000023e4..0x000024ae):
 *
 * Frame 1 (0x14e25460, 8B): u16 scaled current setpoint = (480000*setpoint +
 * 0x8000)>>16; bytes 2..3 = (voltage*28000 + 0x8000) >> 16/24; bytes 4..7 =
 * the packed setpoint limits 0x07d0d930. Frame 2 (0x14e2b462, 2B): a u16 read.
 * Frame 3 (0x14e25461, 1B): the charge mode. Frame 4 (0x14e23460, 6B): packed
 * status — byte0 = status & 0xbf, byte1 = nibble_a & 0x07, byte2 = 0,
 * byte3 = nibble_b & 0x0f, byte4 = 0, byte5 = status_bits & 0x7f.
 *
 * The two incoming args seed the frame buffer but are fully overwritten (dead).
 */
void charger_build_can_reports(uint32_t arg0, uint32_t arg1)
{
    uint8_t  buf[8] __attribute__((aligned(4)));
    int      setpoint, v;
    uint32_t vv;

    (void)arg0;
    (void)arg1;

    /* frame 1 — current setpoint + voltage + packed limits */
    setpoint = (int)charger_compute_charge_current_setpoint();
    *(uint16_t *)&buf[0] =
        (uint16_t)((CHG_RPT_CUR_SCALE * (uint32_t)setpoint + 0x8000) >> 16);
    v  = charge_read_19e0();
    vv = (uint32_t)(v * 28000) + 0x8000;
    buf[2] = (uint8_t)(vv >> 16);
    buf[3] = (uint8_t)(vv >> 24);
    *(uint32_t *)&buf[4] = CHG_RPT_LIMITS;
    charge_tx_enqueue(CHG_RPT_ID_1, 8, buf);

    /* frame 2 — u16 value */
    *(uint16_t *)&buf[0] = (uint16_t)charge_read_1a74();
    charge_tx_enqueue(CHG_RPT_ID_2, 2, buf);

    /* frame 3 — charge mode */
    buf[0] = (uint8_t)charge_read_mode();
    charge_tx_enqueue(CHG_RPT_ID_3, 1, buf);

    /* frame 4 — packed status nibbles */
    buf[0] = (uint8_t)(charge_read_3394() & 0xbf);
    buf[1] = (uint8_t)(charge_read_nibble_a() & 0x07);
    buf[2] = 0;
    buf[3] = (uint8_t)(charge_read_nibble_b() & 0x0f);
    buf[4] = 0;
    buf[5] = (uint8_t)(charge_read_status_bits() & 0x7f);
    charge_tx_enqueue(CHG_RPT_ID_4, 6, buf);
}
