/*
 * ble_msg.c — VanMoof BLE message transport (TX side).
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions:
 *   ble_msg_tx_busy_get   @ 0x0003f2cc  (read TX in-flight flag)
 *   ble_msg_tx_busy_clear @ 0x0003f2d8  (clear TX in-flight flag)
 */

#include "ble.h"

/*
 * ble_msg_tx_busy_get — read the TX in-flight/channel-busy flag. // 0x0003f2cc
 *
 * OEM disassembly (0x3f2cc..0x3f2d0):
 *
 * Guards message sends: a new frame is only queued while this is clear.
 */
uint8_t ble_msg_tx_busy_get(void)
{
    return BLE_TX_BUSY;
}

/*
 * ble_msg_tx_busy_clear — clear the TX in-flight flag. // 0x0003f2d8
 *
 * OEM disassembly (0x3f2d8..0x3f2de):
 *
 * Called before queueing a new message so the next send is allowed through.
 */
void ble_msg_tx_busy_clear(void)
{
    BLE_TX_BUSY = 0;
}
