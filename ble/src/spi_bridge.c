/*
 * spi_bridge.c — VanMoof BLE <-> main-ECU comm-port (SPI-bridge) glue.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   spi_bridge_unlock           @ 0x0003ef10  (release the comm-port lock)
 *   spi_bridge_consumer_thread  @ 0x0003ef1c  (DEFERRED — see docs/progress.md)
 *
 * The consumer thread (0x0003ef1c) owns the host<->BLE comm-port: it deframes the
 * 0x55AA/CRC16 packets the host sends, maintains a two-slot 0x7de-byte transmit
 * double-buffer, runs the window/flow-control, and spills inbound payloads to a
 * flash-writer message queue. Its VanMoof framing/CRC/window logic is understood,
 * but its body is dominated by vendor Zephyr k_poll / k_pipe / ring-buffer
 * plumbing whose stack-struct layout and inbound-length dataflow are not yet
 * pinned down. It is intentionally NOT reconstructed here (flagged in progress.md)
 * rather than shipped as a speculative reconstruction.
 */

#include "ble.h"

/*
 * spi_bridge_unlock — release the comm-port (SPI-bridge) lock. // 0x0003ef10
 *
 * OEM disassembly (0x0003ef10..0x0003ef1a):
 *
 * Thin thunk: loads the comm-port lock object (BLE_COMM_LOCK, 0x20001e38) and
 * tail-calls the vendor lock-release primitive (0x0004f568, a Zephyr-style
 * give/unblock that pops the waiter and re-schedules). No return value.
 */
void spi_bridge_unlock(void)
{
    comm_lock_release_4f568((void *)BLE_COMM_LOCK);
}
