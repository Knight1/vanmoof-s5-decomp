/*
 * ble_connect.c — VanMoof BLE connect / advertising state.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions:
 *   ble_connect_clear_adv_flag @ 0x0003e588  (clear connect-state byte)
 *   ble_connect_set_ready_flag @ 0x0003e59c  (set connect-state byte = 1)
 *
 * Both gate the single connect/advertising state byte (BLE_CONNECT_STATE,
 * 0x20007ef5) on a vendor link-state check, only updating it when the check
 * returns 0.
 */

#include "ble.h"

/*
 * ble_connect_clear_adv_flag — clear the connect/advertising state. // 0x0003e588
 *
 * OEM disassembly (0x3e588..0x3e594):
 *
 * The store reuses r0 (which is 0 when the branch is not taken), i.e. it writes
 * the literal 0 — reproduced here as an explicit `= 0`.
 */
void ble_connect_clear_adv_flag(void)
{
    if (ble_link_check_51b9c() == 0) {
        BLE_CONNECT_STATE = 0;
    }
}

/*
 * ble_connect_set_ready_flag — mark the connect state ready. // 0x0003e59c
 *
 * OEM disassembly (0x3e59c..0x3e5aa):
 */
void ble_connect_set_ready_flag(void)
{
    if (ble_link_check_51b7c() == 0) {
        BLE_CONNECT_STATE = 1;
    }
}
