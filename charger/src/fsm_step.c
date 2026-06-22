/*
 * fsm_step.c — charger main charge state-machine step (the ~11-state FSM).
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). Run periodically when the
 * gate byte's trigger bit is set: debounces the charge-present substate, folds
 * the CAN status into a telemetry report word (sent on change), handles the
 * gate fault bit, then advances the charge state machine (states 0/1/2/3/5/10)
 * with per-state timeout counters. Translated from the OEM image (raw ARM
 * Cortex-M, base 0x0); the register/measurement/Tx helpers are vendor. The
 * goto structure mirrors the OEM control flow (shared tails / cross-state edges).
 */

#include "charger.h"

/* RAM globals (resolved from the literal pool at 0x3634/0x3710). */
#define CHG_STATE_FLAGS 0x20000b06u  /* u8 charge-active flag/status byte */
#define CHG_GATE        0x20000b04u  /* gate/trigger byte (bit0 run, bit1 fault) */
#define CHG_SUBSTATE    0x20000b02u  /* u8 charge-present substate (0/1) */
#define CHG_REPORT      0x20000968u  /* u32 telemetry report word */
#define CHG_REPORT_PREV 0x20000964u  /* u32 last reported word */
#define CHG_MEAS        0x20000ac0u  /* u16 measured status word */
#define CHG_MEAS_PREV   0x20000abeu  /* u16 last reported measured word */
#define CHG_CNT_AB0     0x20000ab0u  /* u16 charge-present debounce counter */
#define CHG_CNT_95C     0x2000095cu  /* u32 per-state timeout counter */
#define CHG_CNT_954     0x20000954u  /* u32 state-1 counter */
#define CHG_CNT_958     0x20000958u  /* u32 state-10 counter */
#define CHG_FLAG_960    0x20000960u  /* u32 state-2 sub-flag */
#define CHG_THRESH_954  120511u      /* state-1 timeout threshold (0x1d4bf) */

void charger_charge_state_machine_step(void)
{
    volatile uint8_t  *substate = (volatile uint8_t *)CHG_SUBSTATE;
    volatile uint8_t  *status   = (volatile uint8_t *)CHG_STATE_FLAGS; /* 0x20000b06 */
    volatile uint8_t  *state    = (volatile uint8_t *)CHG_STATE_BYTE;  /* 0x20000b05 */
    volatile uint32_t *report   = (volatile uint32_t *)CHG_REPORT;
    uint8_t  gate = *(volatile uint8_t *)CHG_GATE;
    uint8_t  b;
    uint16_t cnt16;
    uint32_t cnt;            /* uVar10 */
    volatile uint32_t *cntp; /* puVar13 */
    int disp;                /* uVar12 */
    int s, m, lim;

    if ((int)((uint32_t)gate << 0x1f) >= 0)   /* trigger bit clear */
        return;
    *(volatile uint8_t *)CHG_GATE = 0;

    /* --- charge-present debounce -> substate --- */
    s = charge_read_state();
    if (s == 1 || charge_read_state() == 3 || charge_read_state() == 4) {
        *substate = 1;
    } else if (*substate == 0) {
        if (charge_query_chg(0, 0x1a) == 1) {
            cnt16 = *(volatile uint16_t *)CHG_CNT_AB0;
            if (cnt16 < 100) {
                *(volatile uint16_t *)CHG_CNT_AB0 = (uint16_t)(cnt16 + 1);
                if (cnt16 == 99) { *substate = 1; *(volatile uint16_t *)CHG_CNT_AB0 = 0; }
            }
        } else {
            *(volatile uint16_t *)CHG_CNT_AB0 = 0;
        }
    } else if (*substate == 1) {
        if (charge_query_chg(0, 0x1a) != 0) {
            *(volatile uint16_t *)CHG_CNT_AB0 = 0;
        } else {
            cnt16 = *(volatile uint16_t *)CHG_CNT_AB0;
            if (cnt16 < 100) {
                *(volatile uint16_t *)CHG_CNT_AB0 = (uint16_t)(cnt16 + 1);
                if (cnt16 == 99) {
                    charger_charge_state_dispatch(1);
                    charger_charge_step_finalize();
                    *substate = 0;
                    *(volatile uint16_t *)CHG_CNT_AB0 = 0;
                    charge_status_cmd(0xffff);
                    charge_event_post(0, 0x0d, 1);
                }
            }
        }
    }

    /* --- debounce status bit 7 (fault active) from the CAN status bits --- */
    b = *status;
    if ((int8_t)b < 0) {                       /* bit7 set */
        uint8_t sb = (uint8_t)charge_read_status_bits();
        if ((sb & 0x3f) == 0)
            *status = (uint8_t)((b & 0x7f) | (sb << 7));
    } else {                                   /* bit7 clear */
        if ((charge_read_status_bits() & 0x3f) != 0)
            *status = (uint8_t)(b | 0x80);
    }

    /* --- assemble the telemetry report word; send on change --- */
    *report = (*status & 0xbf) | *report;
    *report = ((charge_read_nibble_a() & 0xf) << 8)  | *report;
    *report = ((charge_read_nibble_b() & 0xf) << 24) | *report;
    {
        uint16_t sbits = (uint16_t)charge_read_status_bits();
        uint16_t meas  = (uint16_t)(((sbits & 0x1f) << 8) | *(volatile uint16_t *)CHG_MEAS);
        uint32_t rep   = *report;
        *(volatile uint16_t *)CHG_MEAS = meas;
        if (rep != *(volatile uint32_t *)CHG_REPORT_PREV ||
            *(volatile uint16_t *)CHG_MEAS_PREV != meas) {
            *(volatile uint16_t *)CHG_MEAS_PREV = meas;
            *(volatile uint32_t *)CHG_REPORT_PREV = rep;
            charge_send_report(4);
        }
    }

    /* --- gate fault bit -> stop --- */
    if ((int)((uint32_t)gate << 0x1e) < 0) {   /* bit1 set */
        charger_charge_step_finalize();
        charger_charge_state_dispatch(10);
        charge_status_cmd(2);
        return;
    }

    /* --- state machine --- */
    if (*state == 1)
        goto case1;
    if (*state == 10)
        goto case10;

    if ((charge_read_status_bits() & 0x3f) != 0) {
        charger_charge_step_finalize();
        charger_charge_state_dispatch(1);
    }
    switch (*state) {
    case 0:
        cnt = *(volatile uint32_t *)CHG_CNT_95C;
        *(volatile uint32_t *)CHG_CNT_95C = cnt + 1;
        if (cnt + 1 < 100)
            return;
        disp = 1;
        goto dispatch;
    case 1:
        goto case1;
    case 2:
        if (charge_read_meas() == 0) {
            charger_charge_step_finalize();
            charge_status_cmd(2);
            charger_report_fault_event();
            disp = 3;
            goto dispatch;
        }
        cnt = *(volatile uint32_t *)CHG_CNT_95C;
        cntp = (volatile uint32_t *)CHG_CNT_95C;
        if (cnt > 99) {
            if (charge_query_2ca8() == 0) {
                if (*(volatile uint32_t *)CHG_FLAG_960 == 0) {
                    *(volatile uint32_t *)CHG_FLAG_960 = 1;
                    return;
                }
                charge_status_cmd(2);
                charge_clear_output(1);
                return;
            }
            if (charge_read_mode() != 0x0d)
                return;
            if (charge_read_meas() == charge_get_limit())
                return;
            m = charge_read_meas();
            lim = charger_get_version_id();
            if ((uint32_t)m < (uint32_t)lim) {
                charger_charge_event_dispatch(0x10);
                charge_status_cmd(2);
                charge_clear_output(0);
            } else {
                charger_charge_event_dispatch(8);
            }
            disp = 5;
            goto dispatch;
        }
        break;
    case 3:
        cnt = *(volatile uint32_t *)CHG_CNT_95C;
        cntp = (volatile uint32_t *)CHG_CNT_95C;
        if (cnt > 99) {
            if (charge_read_meas() == 0)
                return;
            goto dispatch2;
        }
        break;
    default:
        return;
    case 5:
        if (charge_read_mode() == 0x0d) {
            if (charge_read_meas() == charge_get_limit()) {
                charge_status_cmd(2);
                *state = 2;
                *(volatile uint8_t *)CHG_COMPLETE_FLAG = 0;
            }
        }
        if (charge_read_meas() != 0 && *(volatile uint8_t *)CHG_COMPLETE_FLAG != 1)
            return;
        cnt = *(volatile uint32_t *)CHG_CNT_95C + 1;
        *(volatile uint32_t *)CHG_CNT_95C = cnt;
        if (cnt > 99) {
            charge_status_cmd(2);
            charger_charge_state_dispatch(2);
            *(volatile uint8_t *)CHG_COMPLETE_FLAG = 0;
            return;
        }
        if (cnt != 10)
            return;
        goto finalize_ret;
    case 10:
        goto case10;
    }
    *cntp = cnt + 1;
    return;

case1:
    cnt = *(volatile uint32_t *)CHG_CNT_954;
    *(volatile uint32_t *)CHG_CNT_954 = cnt + 1;
    if (CHG_THRESH_954 < cnt)
        charge_event_post(0, 0x0d, 1);
    cntp = (volatile uint32_t *)CHG_CNT_95C;
    if (((*substate != 1) || ((charge_read_status_bits() & 0x3f) != 0)) &&
        (charge_read_state() != 4)) {
        cnt = 0;
        goto store_ret;
    }
    cnt = *cntp;
    if (cnt < 100) {
        cnt = cnt + 1;
        goto store_ret;
    }
    if (charge_read_state() == 4)
        charge_status_cmd(0xffff);
dispatch2:
    disp = 2;
dispatch:
    charger_charge_state_dispatch(disp);
    return;
store_ret:
    *cntp = cnt;
    return;

case10:
    if ((*(volatile uint16_t *)CHG_MODE_WORD == 0) || (charge_query_1c54() == 1)) {
        cnt = *(volatile uint32_t *)CHG_CNT_958;
        *(volatile uint32_t *)CHG_CNT_958 = cnt + 1;
        if (cnt != 0) {
            if (cnt + 1 < 0x65)
                return;
            charger_charge_state_dispatch(2);
            charge_helper_1c48(0);
            return;
        }
        goto finalize_ret;
    }
    cnt = *(volatile uint32_t *)CHG_CNT_95C;
    cntp = (volatile uint32_t *)CHG_CNT_95C;
    if (cnt > 99) {
        charger_charge_event_dispatch(2);
        return;
    }
    *cntp = cnt + 1;
    return;

finalize_ret:
    charger_charge_step_finalize();
    return;
}
