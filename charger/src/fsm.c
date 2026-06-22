/*
 * fsm.c — charger charge state-machine dispatch.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). charger_charge_state_dispatch
 * drives a state transition: it resets the working accumulators, sets the new
 * state byte, and runs the entry actions for states 1 (init), 2 (charging),
 * 5 (charging-active flags) and 10 (fault). Translated from the OEM image (raw
 * ARM Cortex-M, base 0x0). The event-post / M_CAN-Tx / status helpers are vendor;
 * the transition policy is the charger app.
 *
 * NOTE: the M_CAN-Tx arm in the state-2 (charging) entry carries the variant
 * parameter charge_arm_tx(0x69) — 0x09 in the `speed` build (see docs/variants.md).
 */

#include "charger.h"

/* RAM work-globals reset on every transition (resolved from the literal pool). */
#define CHG_WORK_954   0x20000954u  /* u32 (reset in state 1) */
#define CHG_WORK_958   0x20000958u  /* u32 */
#define CHG_WORK_95C   0x2000095cu  /* u32 */
#define CHG_WORK_960   0x20000960u  /* u32 */
#define CHG_WORK_AB0   0x20000ab0u  /* u16 */
#define CHG_STATE_FLAGS 0x20000b06u /* u8 charge-active flag byte */

/* charge-event-dispatch globals. */
#define CHG_EVT_FLAGS  0x20000a94u  /* u8[3]: [0] started, [1], [2] */
#define CHG_EVT_AFC    0x20000afcu  /* u8 (code 1) */
#define CHG_EVT_STATE  0x20000afbu  /* u8 event-state (codes 2/4/0x10) */
#define CHG_EVT_AFA    0x20000afau  /* u8 (code 0x10) */
#define CHG_EVT_HW     0x20000020u  /* u16 (code 0x10) */

/* charger_charge_state_dispatch — perform a charge state transition.
 *
 * OEM disassembly (0x00003150..0x0000322a):
 *
 * Zeroes the working accumulators and stores the new state. State 1 (init) posts
 * the stop event, clears the outputs and both setpoints, and reports a fault
 * event. State 2 (charging) zeroes the scaled setpoint, raises status command 2,
 * posts the start/stop events, dispatches the charge event, selects the per-state
 * setpoint, then either arms the M_CAN Tx (charge_arm_tx(0x69)) on success or
 * notifies + posts a stop event on failure, and sets flag bit 1. State 5 sets
 * the active bit 0x20 and returns; other states clear the low flag bits. State 10
 * (fault) additionally reports the fault event.
 */
void charger_charge_state_dispatch(int new_state)
{
    volatile uint8_t *flags = (volatile uint8_t *)CHG_STATE_FLAGS;
    uint8_t b;

    *(volatile uint32_t *)CHG_WORK_95C = 0;
    *(volatile uint32_t *)CHG_WORK_958 = 0;
    *(volatile uint16_t *)CHG_WORK_AB0 = 0;
    *(volatile uint32_t *)CHG_WORK_960 = 0;
    *(volatile uint8_t  *)CHG_STATE_BYTE = (uint8_t)new_state;

    if (new_state == 1) {
        *(volatile uint32_t *)CHG_WORK_954 = 0;
        charge_event_post(1, 5, 0);
        charge_clear_output(0);
        charger_set_charge_setpoint(0);
        charger_set_charge_scaled_setpoint(0);
        charger_report_fault_event();
        charge_helper_1cdc();
    } else if (new_state == 2) {
        charger_set_charge_scaled_setpoint(0);
        charge_status_cmd(2);
        charge_event_post(1, 5, 1);     /* OEM 0x31bc: r2=1 (NOT 0; state-1 path uses 0) */
        charge_event_post(0, 0x0d, 0);
        charge_helper_1320();
        charger_charge_event_dispatch(2);
        charger_select_charge_setpoint_by_state();
        if (charge_read_arm_flag() == 0) {
            charge_arm_tx(0x69);            /* variant param (0x09 in speed) */
            charge_helper_16b0();
            charge_helper_1cf0();
        } else {
            charge_helper_1d18();
            charge_helper_1cf0();
            charge_notify_state();
            charge_event_post(0, 0x16, 1);
        }
        *flags |= 2;
        goto clear_charge_bits;
    }

    b = *flags;
    *flags = (uint8_t)(b & 0xfc);
    if (new_state == 5) {
        *flags = (uint8_t)((b & 0xfc) | 0x20);
        return;
    }

clear_charge_bits:
    *flags &= 0xcf;
    if (new_state != 10)
        return;
    charger_report_fault_event();
    charge_helper_1cdc();
}

/* charger_charge_event_dispatch — handle a charge event-code bit (1/2/4/8/0x10).
 *
 * OEM disassembly (0x00002ac0..0x00002b42):
 *
 *   1     -> set bit 0 of the AFC flag byte
 *   2     -> if the started flag is clear, set it and the event-state to 1
 *   4     -> if started, set event-state 9 and post the start event (1,5,0)
 *   8     -> set the second started flag if clear
 *   0x10  -> set event-state 10, write 0x30 to the HW word, arm the M_CAN Tx
 *            (charge_arm_tx(0x30)), clear the AFA flag and set started[2]
 * Other codes are ignored.
 */
void charger_charge_event_dispatch(int code)
{
    volatile uint8_t *flags = (volatile uint8_t *)CHG_EVT_FLAGS;

    if (code == 1) {
        *(volatile uint8_t *)CHG_EVT_AFC |= 1u;
    } else if (code == 2) {
        if (*flags != 0)
            return;
        *flags = 1;
        *(volatile uint8_t *)CHG_EVT_STATE = 1;
    } else if (code == 4) {
        if (*flags == 1) {
            *(volatile uint8_t *)CHG_EVT_STATE = 9;
            charge_event_post(1, 5, 0);
        }
    } else if (code == 8) {
        if (flags[1] == 0)
            flags[1] = 1;
    } else if (code == 0x10) {
        *(volatile uint8_t  *)CHG_EVT_STATE = 10;
        *(volatile uint16_t *)CHG_EVT_HW = 0x30;
        charge_arm_tx(0x30);
        *(volatile uint8_t  *)CHG_EVT_AFA = 0;
        flags[2] = 1;
    }
}

/* charger_charge_step_finalize — finish a charge step.
 *
 * OEM disassembly (0x00005078..0x000050a0):
 *
 * Dispatches charge event 4 (start-confirm), runs the post-step helper, clears
 * the output, and posts the fault event (0,0x16,0) if the arm flag is set.
 */
void charger_charge_step_finalize(void)
{
    charger_charge_event_dispatch(4);
    charge_helper_1cdc();
    charge_clear_output(0);
    if (charge_read_arm_flag() == 1)
        charge_event_post(0, 0x16, 0);
}
