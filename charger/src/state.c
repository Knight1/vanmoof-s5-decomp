/*
 * state.c — charger charge state machine: status flags, mode/state transitions,
 * per-state setpoint selection, and control-context initializers.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). These functions drive the
 * charge state byte (idle/init/ready/pre-charge/charging) and pick the hardware
 * setpoint per state. Translated from the OEM image (raw ARM Cortex-M, base 0x0).
 * The register/CAN accessors (charge_read_*) and event helpers are vendor; the
 * state policy and setpoint selection are the charger app.
 *
 * NOTE: the per-state setpoint and the M_CAN-Tx arming carry the charge-control
 * parameter 0x69 that distinguishes the `normal` build from `speed` (0x09); see
 * docs/variants.md. It appears here as the `* -0x69` scale in state 2.
 */

#include "charger.h"

/* charger_set_charge_status_flag — update the charge status-flag byte.
 *
 * OEM disassembly (0x00003270..0x00003318):
 *
 * OR-sets the command bit into the status byte, then for commands 8 / 0x10 / 0x20
 * masks off the paired bit (0x10 / 0x08 / 0x20). On the 0x20 command, after
 * clearing it checks the aggregate CAN/status bits (charge_read_status_bits() &
 * 0x3f) and the charge state — when both are clear and the state is 5
 * (charging), it raises the charge-complete flag.
 */
void charger_set_charge_status_flag(int cmd)
{
    volatile uint8_t *status = (volatile uint8_t *)CHG_STATUS_BYTE;
    uint8_t v = (uint8_t)(*status | (uint8_t)cmd);

    if (cmd == 8) {
        v &= 0xef;
    } else if (cmd == 0x10) {
        v &= 0xf7;
    } else if (cmd == 0x20) {
        *status = v & 0xdf;
        if ((charge_read_status_bits() & 0x3f) != 0)
            return;
        if (*(volatile uint8_t *)CHG_STATE_BYTE != 5)
            return;
        *(volatile uint8_t *)CHG_COMPLETE_FLAG = 1;
        return;
    }
    *status = v;
}

/* charger_charge_state5_to_state2 — tear down an active charge (state 5 ->2).
 *
 * OEM disassembly (0x00003724..0x00003752):
 *
 * When charging (state 5), runs the stop sequence (teardown, post mode 0x0d,
 * clear output, zero the scaled setpoint), drops to state 2 (ready) and notifies.
 */
void charger_charge_state5_to_state2(void)
{
    volatile uint8_t *state = (volatile uint8_t *)CHG_STATE_BYTE;

    if (*state == 5) {
        charge_teardown_step();
        charge_post_mode(0x0d);
        charge_clear_output(0);
        charger_set_charge_scaled_setpoint(0);
        *state = 2;
        charge_notify_state();
    }
}

/* charger_set_charge_mode — update the charge mode/state word.
 *
 * OEM disassembly (0x00003758..0x00003772):
 *
 * If bits 0x18 of the mode word change, raises status command 2 first, then
 * stores the new mode word.
 */
void charger_set_charge_mode(uint16_t mode)
{
    volatile uint16_t *modeword = (volatile uint16_t *)CHG_MODE_WORD;

    if (((*modeword ^ mode) & 0x18) != 0)
        charger_set_charge_status_flag(2);
    *modeword = mode;
}

/* charger_select_charge_setpoint_by_state — pick the HW setpoint for the state.
 *
 * OEM disassembly (0x00002b7c..0x00002be6):
 *
 * Reads the charge state and writes the per-state intermediate value to
 * CHG_SETPOINT_RAW and the secondary HW field (CHG_SETPOINT_CTX+4):
 *   state 0 -> {0xdece, 0x12a7}
 *   state 1 / 4 -> {0x8613, 0x1da2}
 *   state 2 -> raw = (v * -0x69 >> 7) + 0x138ee, then set_charge_setpoint(v)
 *   state 3 -> {0x9e6e, 0x1995}
 * where v is the measured value (charge_read_value16). The `-0x69` scale is the
 * normal/speed variant parameter.
 */
void charger_select_charge_setpoint_by_state(void)
{
    volatile uint16_t *raw = (volatile uint16_t *)CHG_SETPOINT_RAW;
    volatile uint16_t *ctx = (volatile uint16_t *)(CHG_SETPOINT_CTX + 4);
    int state = charge_read_state();

    if (state == 0) {
        *raw = 0xdece;
        *ctx = 0x12a7;
    } else if (state == 2) {
        int v = charge_read_value16();
        *raw = (uint16_t)((v * -0x69 >> 7) + 0x138ee);
        charger_set_charge_setpoint(v);
    } else if (state == 3) {
        *raw = 0x9e6e;
        *ctx = 0x1995;
    } else if (state == 1 || state == 4) {
        *raw = 0x8613;
        *ctx = 0x1da2;
    }
}

/* charger_charge_ctx_init — seed the 9-byte charge control context.
 *
 * OEM disassembly (0x00004f04..0x00004f28):
 */
void charger_charge_ctx_init(void *ctx)
{
    uint8_t *p = (uint8_t *)ctx;

    mem_set(p, 0, 9);
    p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0; p[6] = 0;
    p[0] = 1;
    p[7] = 1;
    p[8] = 0x0f;
}

/* charger_ctx_init — zero the 16-byte charger control struct.
 *
 * OEM disassembly (0x00004b9c..0x00004bb4):
 */
void charger_ctx_init(void *ctx)
{
    uint8_t *p = (uint8_t *)ctx;

    mem_set(p, 0, 0x10);
    *(uint32_t *)(p + 0x00) = 0;
    *(uint32_t *)(p + 0x04) = 0;
    *(uint32_t *)(p + 0x08) = 0;
    *(uint16_t *)(p + 0x0c) = 0;
    *(uint8_t  *)(p + 0x0e) = 0;
}

/* charger_can_msg_init — initialize a 0x24-byte CAN-message / charger object.
 *
 * OEM disassembly (0x00004c32..0x00004c58):
 *
 * Zeroes the struct and seeds the length/flags field at +0x14 with 0x0700.
 */
void charger_can_msg_init(void *msg)
{
    uint8_t *p = (uint8_t *)msg;

    mem_set(p, 0, 0x24);
    *(uint32_t *)(p + 0x04) = 0;
    *(uint32_t *)(p + 0x08) = 0;
    *(uint32_t *)(p + 0x18) = 0;
    *(uint32_t *)(p + 0x1c) = 0;
    p[0x00] = 0;
    p[0x0c] = 0;
    *(uint32_t *)(p + 0x10) = 0;
    *(uint16_t *)(p + 0x14) = 0x0700;
    p[0x16] = 0;
    *(uint16_t *)(p + 0x20) = 0;
}

/* charger_charge_enable_set — arm a charge on a CAN object.
 *
 * OEM disassembly (0x00004e76..0x00004eca):
 *
 * If the state slot (state_ctx+0x14d) is idle, marks it 5 (charging), latches
 * the requested setpoint at +0x108, sets the enable bit on the device, and
 * programs the per-mode register pair (+0x54 / +0x58). If already busy, returns
 * the busy code (0x1847 for mode 1, else 0x1846); success returns 0.
 */
uint32_t charger_charge_enable_set(void *dev, int mode, void *state_ctx,
                                   const uint32_t *setpoint)
{
    uint8_t *d = (uint8_t *)dev;
    uint8_t *s = (uint8_t *)state_ctx;

    if (*(char *)(s + 0x14d) == 0) {
        *(uint8_t  *)(s + 0x14d) = 5;
        *(uint32_t *)(s + 0x108) = *setpoint;
        *(uint32_t *)(d + 0x5c) |= 1u;
        if (mode == 1) {
            *(uint32_t *)(d + 0x58) &= 0xffffffefu;
            *(uint32_t *)(d + 0x54) |= 0x10u;
        } else {
            *(uint32_t *)(d + 0x58) &= 0xfffffffeu;
            *(uint32_t *)(d + 0x54) |= 1u;
        }
        return 0;
    }
    return (mode == 1) ? 0x1847u : 0x1846u;
}
