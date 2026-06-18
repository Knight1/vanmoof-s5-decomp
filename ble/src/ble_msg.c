/*
 * ble_msg.c — VanMoof BLE message transport (TX side).
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   ble_msg_send             @ 0x0003f210  (COBS-frame + queue on the comm pipe)
 *   ble_msg_tx_busy_get      @ 0x0003f2cc  (read TX in-flight flag)
 *   ble_msg_tx_busy_clear    @ 0x0003f2d8  (clear TX in-flight flag)
 *   ble_uicr_write_init_flag @ 0x0003f2e4  (one-time UICR provisioning write)
 */

#include "ble.h"

/*
 * ble_msg_send — frame an application message and transmit it on the comm-port
 * pipe (the SPI-bridge link to the main MCU). // 0x0003f210
 *
 * OEM disassembly (0x0003f210..0x0003f297):
 *
 * Takes the comm-port lock; on contention it returns -0x74. With the lock held
 * it COBS-encodes the caller's message (src/len) into the shared framed-packet
 * buffer (capacity 0x406); a negative encoded length (overflow) unlocks and
 * returns -5. Otherwise it writes the encoded frame into the comm-port pipe in a
 * retry loop of up to ten attempts, re-trying while the pipe returns -0xb
 * (would-block); exhausting the retries or any other non-zero result unlocks and
 * returns -0x74. On a successful write it signals the consumer via the notify
 * object; a non-zero notify result unlocks and returns -5, success unlocks and
 * returns 0.
 */
int ble_msg_send(const void *src, uint32_t len)
{
    int      enc_len;
    int      rc;
    int      retries;
    uint32_t written;

    if (ble_comm_lock_take_4f478((void *)BLE_COMM_LOCK, len, 0x148, 0) != 0) {
        return -0x74;
    }

    enc_len = ble_cobs_encode_61c50(src, len, (void *)BLE_COMM_FRAME_BUF, 0x406);
    if (enc_len < 0) {
        spi_bridge_unlock();
        return -5;
    }

    retries = 10;
    do {
        rc = ble_comm_pipe_write_4f7f0((void *)BLE_COMM_PIPE,
                                       (void *)BLE_COMM_FRAME_BUF,
                                       (uint32_t)enc_len, &written,
                                       (uint32_t)enc_len, 0, 0x148, 0);
        if (--retries == 0) {
            break;
        }
    } while (rc == -0xb);

    if (rc != 0) {
        spi_bridge_unlock();
        return -0x74;
    }

    rc = ble_comm_notify_58af4(*(void **)BLE_COMM_NOTIFY_PTR, 1);
    if (rc != 0) {
        spi_bridge_unlock();
        return -5;
    }

    spi_bridge_unlock();
    return 0;
}

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

/*
 * ble_uicr_write_init_flag — one-time provisioning write: if the device-config
 * word at 0x10001208 still reads erased (0xFFFFFFFF), program it via the flash
 * controller. // 0x0003f2e4
 *
 * OEM disassembly (0x0003f2e4..0x0003f30a):
 *
 * Reads the UICR/config word at 0x10001208 and acts only when it is still erased
 * (== 0xFFFFFFFF). It stages the 4-byte value 0xffffff00 on the stack and calls
 * the flash controller's write op (the device at BLE_FLASH_DEV; its driver API
 * at dev->api, write entry api->write) to program 4 bytes at offset 0x10001208.
 * The incoming register arguments are unused (the prologue only reserves stack).
 *
 * (Renamed from the earlier guess "ble_msg_send_init_error": it writes a flash /
 * UICR word, it does not send a message.)
 */
void ble_uicr_write_init_flag(void)
{
    volatile uint32_t word;   /* sp+4 scratch holding the value to program */

    if (BLE_DEVICE_PROVISION_WORD == 0xFFFFFFFFu) {
        word = 0xffffff00u;
        BLE_FLASH_DEV->api->write(BLE_FLASH_DEV, 0x10001208u,
                                  (const void *)&word, 4);
    }
}
