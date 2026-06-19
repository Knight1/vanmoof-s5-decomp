/*
 * spi_bridge.c — VanMoof BLE <-> main-ECU comm-port (SPI-bridge) glue.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   ble.20240129.145222.1.5.0.main.v1.5.0-main.bin   (device link base 0x23000)
 *
 * Functions (OEM address order):
 *   spi_bridge_unlock           @ 0x0003ef10  (release the comm-port lock)
 *   spi_bridge_consumer_thread  @ 0x0003ef1c  (comm-port consumer task)
 *
 * The consumer thread owns the host<->BLE comm-port over the spi@40004000 device:
 * it deframes the 0x55AA/CRC16 packets the host sends, maintains a two-slot
 * 0x7de-byte transmit double-buffer, runs the window/flow-control, and spills
 * inbound payloads to a flash-writer message queue. The Zephyr k_poll / k_sem /
 * ring-buffer / SPI-driver primitives it drives are vendor (declared extern); the
 * framing / CRC / window / dispatch logic is VanMoof.
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

/*
 * spi_bridge_consumer_thread — the comm-port (SPI-bridge) consumer task.
 * // 0x0003ef1c
 *
 * OEM disassembly (0x0003ef1c..0x0003f1d4):
 *
 * Zephyr thread entry (reached via the thread table, not a direct call). At
 * bring-up it initialises the inbound COBS ring over a 0xc1e-byte backing store,
 * zeroes the new-data k_sem, sets up a k_poll_event and two SPI buffer
 * descriptors ({buf, size, &self, 1}) over the spi@40004000 device, publishes the
 * producer notify device into *BLE_COMM_NOTIFY_PTR, programs the static frame
 * descriptor (cfg bytes +0xa=7, +0xb=0xcf, length +0xc/+0xd=0), clears the slot
 * toggle, and drains the SPI source once (retrying on a vendor delay). It then
 * announces every broadcastable command id to the host.
 *
 * The vendor SPI driver DMAs received bytes into the active double-buffer slot
 * and reports the transfer length into the poll event's +0xc word; the loop
 * snapshots that length one iteration later (a double-buffer pipeline). Each pass:
 * take the new-data semaphore, snapshot the reported rx length, re-arm the poll
 * over the *next* slot, and re-poll up to ten times; loop back while the snapshot
 * length is zero.
 *
 * Deframe (using the current slot): the first byte is the frame type and the
 * descriptor's pending TX length (+0xc, big-endian) is snapshotted then cleared.
 *   - Type 0x01 (data): if the buffer holds > 6 bytes and the word at (rx-6) is
 *     the 0xAA55AA55 marker, CRC16 the body (seed 0xffff over rx-2 bytes) and
 *     compare big-endian to the stored CRC at (rx-6+4). On match, stream the
 *     payload (skipping the type byte, length rx-7) into the COBS ring; on a full
 *     ring, decode completed frames into a 0x40a-byte buffer and post each to the
 *     flash-writer queue (0x199a-byte items). Ring init/recovery failure aborts
 *     the thread (its only exit).
 *   - Type 0x10 (window/flow-control): a 16-bit big-endian count (<= 0x7de) below
 *     the pending length slides the TX scratch FIFO left by that many bytes and
 *     shrinks the pending length.
 *
 * Refill: when the pending length is below 0x7ca, top the staged payload up from
 * the TX pipe. Emit: write the pending length big-endian, append the
 * {0x55,0xAA,0x55,0xAA} trailer and a CRC16 (seed 0xffff over len+0x12), advance
 * the slot toggle, and poke the producer notify object.
 *
 * The two SPI buffer descriptors, the k_poll_event and the k_sem are vendor
 * Zephyr structures; we provide their backing store and the OEM's field inits and
 * drive them through the comm_* externs.
 */
void spi_bridge_consumer_thread(void)
{
    uint8_t          *desc     = (uint8_t *)COMM_FRAME_DESC;
    uint8_t          *buf_base = (uint8_t *)COMM_TX_BUF_BASE;
    volatile uint32_t *toggle  = (volatile uint32_t *)COMM_TX_BUF_INDEX;

    /* vendor Zephyr scratch (backing + OEM field inits; the SPI driver fills it).
     * The transfer length the driver reports lands in poll_ev[3] (+0xc). */
    uint32_t spi_buf_a[4];
    uint32_t spi_buf_b[4];
    uint32_t poll_ev[6];
    uint8_t  sem[16];
    uint8_t  ring_backing[0xc1e];
    uint8_t  cobs_buf[0x40a];

    uint32_t rx_len, next, type, pend, refill_off, win, i, crc;
    int      ret, tries, rem, wrote, decoded_len;
    uint8_t *frame, *scratch, *trailer;
    uint16_t v, crc_be, pv;

    /* ---- bring-up ---- */
    comm_ring_init_61b8a((void *)COMM_RING, ring_backing, 0xc1e);
    poll_ev[0] = (uint32_t)poll_ev;      /* k_poll_event dlist node = empty */
    poll_ev[1] = (uint32_t)poll_ev;
    poll_ev[2] = 0;
    poll_ev[3] = 0;                       /* +0xc: rx length (written by the poll) */
    poll_ev[5] = 0;
    vm_memset_61e62(sem, 0, 0x10);

    *(volatile uint32_t *)BLE_COMM_NOTIFY_PTR = COMM_NOTIFY_DEVICE;
    ((uint8_t *)&poll_ev[5])[1] = 1;      /* k_poll_event mode */

    spi_buf_a[0] = (uint32_t)desc;
    spi_buf_a[1] = 0x7de;
    spi_buf_a[2] = (uint32_t)spi_buf_a;
    spi_buf_a[3] = 1;
    spi_buf_b[0] = (uint32_t)buf_base;
    spi_buf_b[1] = 0x7de;
    spi_buf_b[2] = (uint32_t)spi_buf_b;
    spi_buf_b[3] = 1;

    desc[0xa] = 7;
    desc[0xb] = 0xcf;
    desc[0xc] = 0;
    desc[0xd] = 0;
    *toggle = 0;
    poll_ev[5] &= 0xfff81fffu;            /* clear k_poll_event state bits */

    while ((ret = comm_spi_poll((void *)COMM_POLL_DEVICE, COMM_POLL_ARG,
                                &spi_buf_a[2], &spi_buf_b[2], poll_ev)) != 0) {
        comm_delay_503f8(0xccd, 0);
    }

    {
        const ble_cmd_entry_t *e   = (const ble_cmd_entry_t *)BLE_CMD_BROADCAST_TABLE;
        const ble_cmd_entry_t *end = (const ble_cmd_entry_t *)BLE_CMD_BROADCAST_TABLE_END;
        for (; e < end; e++) {
            ble_announce_command_id(e->id);
        }
    }

    /* ---- main loop (never returns; aborts only on an unrecoverable ring error) ---- */
    for (;;) {
        do {
            comm_sem_take_509c4(sem, 1, 0xffffffffu, 0xffffffffu);
            rx_len = poll_ev[3];          /* length the previous poll reported */
            spi_buf_a[0] = (uint32_t)desc;
            next = *toggle + 1;
            if (next > 1) {
                next = 0;
            }
            spi_buf_b[0] = next * 0x7de + (uint32_t)buf_base;   /* next RX slot */
            poll_ev[2] = 0;
            poll_ev[5] &= 0xfff81fffu;
            tries = 10;
            do {
                ret = comm_spi_poll((void *)COMM_POLL_DEVICE, COMM_POLL_ARG,
                                    &spi_buf_a[2], &spi_buf_b[2], poll_ev);
                if (ret == 0) {
                    break;
                }
            } while (--tries != 0);
        } while (rx_len == 0);

        frame = buf_base + (*toggle) * 0x7de;
        type = frame[0];
        refill_off = 0;
        v = *(uint16_t *)(desc + 0xc);
        pend = (uint32_t)(((v & 0xff) << 8) | (v >> 8));
        desc[0xc] = 0;
        desc[0xd] = 0;

        if (type == 0x01) {
            desc[8] = 0;
            desc[9] = 0;
            if (rx_len > 6 &&
                *(uint32_t *)(frame + (rx_len - 6)) == COMM_FRAME_MARKER) {
                crc = comm_crc16_58d7c(0xffff, frame, rx_len - 2);
                crc_be = (uint16_t)(((crc & 0xff) << 8) | ((crc >> 8) & 0xff));
                if (crc_be == *(uint16_t *)(frame + (rx_len - 6) + 4)) {
                    *(uint16_t *)(desc + 8) = crc_be;
                    rem = (int)(rx_len - 7);
                    while (rem > 0) {
                        wrote = comm_ring_write_61d5a((void *)COMM_RING,
                                    (*toggle) * 0x7de + (uint32_t)buf_base + 1, rem);
                        if (wrote < 0) {
                            if (comm_ring_frame_ready_61dc8((void *)COMM_RING) != 0) {
                                return;
                            }
                            comm_ring_init_61b8a((void *)COMM_RING,
                                *(void **)((uint8_t *)COMM_RING + 0x10),
                                *(uint32_t *)((uint8_t *)COMM_RING + 0xc));
                            wrote = comm_ring_write_61d5a((void *)COMM_RING,
                                        (*toggle) * 0x7de + (uint32_t)buf_base + 1, rem);
                            if (wrote < 0) {
                                return;
                            }
                        }
                        while (comm_ring_frame_ready_61dc8((void *)COMM_RING) != 0) {
                            if (comm_ring_cobs_decode_61c74((void *)COMM_RING, cobs_buf,
                                                            0x40a, &decoded_len) != 0) {
                                comm_ring_init_61b8a((void *)COMM_RING,
                                    *(void **)((uint8_t *)COMM_RING + 0x10),
                                    *(uint32_t *)((uint8_t *)COMM_RING + 0xc));
                            }
                            comm_msgq_put_4f318((void *)FLASH_WRITE_MSGQ,
                                                cobs_buf, 0x199a, 0);
                        }
                        rem -= wrote;
                    }
                }
            }
            goto lab_f084;
        }

        if (type == 0x10) {
            win = ((uint32_t)frame[1] << 8) | frame[2];
            if (win <= 0x7de) {
                if (win < pend) {
                    scratch = (uint8_t *)COMM_TX_SCRATCH;
                    for (i = 0; (int)i < (int)(pend - win); i++) {
                        scratch[i] = scratch[i + win];
                    }
                    pend = (pend - win) & 0xffff;
                    goto lab_f084;
                }
                goto lab_f08c;            /* win >= pend: refill from offset 0 */
            }
            /* win > 0x7de: fall through to lab_f084 */
        }

    lab_f084:
        refill_off = pend;
        if (pend >= 0x7ca) {
            goto emit;
        }

    lab_f08c:
        decoded_len = 0;
        comm_pipe_read_610e6((void *)BLE_COMM_PIPE,
                             desc + ((refill_off + 0xe) & 0xffff),
                             0x7ca - refill_off, &decoded_len, 1);
        pend = (refill_off + (uint32_t)decoded_len) & 0xffff;

    emit:
        pv = (uint16_t)(((pend & 0xff) << 8) | (pend >> 8));
        *(uint16_t *)(desc + 0xc) = pv;
        if (pend != 0) {
            trailer = desc + pend + 0xe;
            trailer[0] = 0x55;
            trailer[1] = 0xaa;
            trailer[2] = 0x55;
            trailer[3] = 0xaa;
            crc = comm_crc16_58d7c(0xffff, desc, pend + 0x12);
            *(uint16_t *)(trailer + 4) =
                (uint16_t)(((crc & 0xff) << 8) | ((crc >> 8) & 0xff));
        }

        next = *toggle + 1;
        if (next > 1) {
            next = 0;
        }
        *toggle = next;
        ble_comm_notify_58af4(*(void **)BLE_COMM_NOTIFY_PTR, pend != 0 ? 1 : 0);
    }
}
