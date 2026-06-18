/*
 * vm_can.h — VanMoof `vm` library CAN transport (reconstructed)
 *
 * Reconstructed from the i.MX8 `power` service (/usr/bin/power, AArch64 C++,
 * stripped). The `vm` library transports the bike's CAN traffic over Linux
 * raw SocketCAN. The 29-bit-ID bit-packing below is byte-for-byte VERIFIED
 * against the binary (independent objdump/capstone). See main/docs/can-bus.md.
 *
 * Behaviour-oriented C reconstruction — not byte-identical to the OEM C++.
 */
#ifndef VM_CAN_H
#define VM_CAN_H

#include <stdint.h>
#include <pthread.h>

/*
 * vm_address — four fields packed into a 29-bit extended CAN id:
 *   can_id = (a0<<21) | (a1<<13) | (a2<<5) | (a3 & 0x1F)   [| CAN_EFF_FLAG on wire]
 * a0 = node/device id, a1 = signal index, a2 = port/class, a3 = 5-bit sub-id.
 */
struct vm_address {
    uint8_t a0;     /* bits 28..21 */
    uint8_t a1;     /* bits 20..13 */
    uint8_t a2;     /* bits 12..5  */
    uint8_t a3;     /* bits  4..0  (5-bit) */
};

/* In-process frame the transport hands to / takes from the OD layer:
 * a 13-byte record { a0, a1, a2, a3, len, data[0..7] }. */
struct vm_frame {
    uint8_t a0, a1, a2, a3;
    uint8_t len;            /* DLC, 0..8 */
    uint8_t data[8];
};

/* The CAN backend object (lives at vm_s+0x2828 in the binary). */
struct vm_can_backend {
    /* +0x00 */ void (*dispatch)(struct vm_can_backend *self, const struct vm_frame *f);
    /* …      */
    /* +0x20 */ void (*tx)(struct vm_can_backend *self, const struct vm_frame *f); /* = vm_can_tx */
    /* +0x28 */ int *sock;   /* 0x10-byte socket holder; first u32 is the fd */
    pthread_t rx_thread;
};

/* id <-> address (the verified bit-packing). */
uint32_t vm_can_id_encode(const struct vm_address *a);   /* adds CAN_EFF_FLAG */
void     vm_can_id_decode(uint32_t can_id, struct vm_frame *out);

/* OEM 0x158d30: open raw SocketCAN on `ifname` (default "vcan0"), bind, and
 * spawn the RX thread. Returns 0 on success. */
int  vm_can_open(struct vm_can_backend *be, const char *ifname);

/* OEM 0x158b60: build a Linux can_frame from `f` and write() it. */
void vm_can_tx(struct vm_can_backend *be, const struct vm_frame *f);

/* OEM 0x158c30: RX thread — read() loop; decode each 29-bit EFF frame into a
 * vm_frame and hand it to be->dispatch(). */
void *vm_can_rx_thread(void *be);

#endif /* VM_CAN_H */
