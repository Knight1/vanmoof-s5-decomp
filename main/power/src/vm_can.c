/*
 * vm_can.c — VanMoof `vm` library CAN transport (reconstructed)
 *
 * OEM: /usr/bin/power  (Ghidra program "power", image base 0x100000)
 *   vm_can_open       0x158d30
 *   vm_can_tx         0x158b60   (bit-packing VERIFIED via objdump)
 *   vm_can_rx_thread  0x158c30   (id decode VERIFIED via objdump)
 *
 * Faithful translation of the decompiled logic. This is real, standard
 * SocketCAN code and compiles per-TU against the Linux CAN headers.
 */
#define _GNU_SOURCE
#include "vm_can.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* ---- id <-> vm_address ----------------------------------------------- */

/* OEM 0x158b60 (TX): can_id = (a0<<21)|(a1<<13)|(a2<<5)|(a3 & 0x1F) | EFF. */
uint32_t vm_can_id_encode(const struct vm_address *a)
{
    return ((uint32_t)a->a0 << 21)
         | ((uint32_t)a->a1 << 13)
         | ((uint32_t)a->a2 << 5)
         | ((uint32_t)a->a3 & 0x1F)
         | CAN_EFF_FLAG;
}

/* OEM 0x158c30 (RX): a0=id>>21, a1=id>>13, a2=id>>5, a3=id & 0x1F. */
void vm_can_id_decode(uint32_t can_id, struct vm_frame *out)
{
    out->a0 = (uint8_t)(can_id >> 21);
    out->a1 = (uint8_t)(can_id >> 13);
    out->a2 = (uint8_t)(can_id >> 5);
    out->a3 = (uint8_t)(can_id & 0x1F);
}

/* ---- transport ------------------------------------------------------- */

/* OEM 0x158d30. socket(AF_CAN, SOCK_RAW, CAN_RAW) -> SIOCGIFINDEX -> bind ->
 * spawn the RX thread. */
int vm_can_open(struct vm_can_backend *be, const char *ifname)
{
    struct ifreq ifr;
    struct sockaddr_can addr;

    be->tx = vm_can_tx;                          /* installed at backend+0x20 */

    be->sock = malloc(sizeof(int) * 4);          /* 0x10-byte socket holder   */
    if (!be->sock)
        return -1;

    be->sock[0] = socket(PF_CAN, SOCK_RAW, CAN_RAW);   /* AF_CAN=0x1d,3,1 */
    if (be->sock[0] < 0) {
        free(be->sock);
        be->sock = NULL;
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname ? ifname : "vcan0", IFNAMSIZ);  /* 0x10 bytes */
    ioctl(be->sock[0], SIOCGIFINDEX, &ifr);                      /* 0x8933 */

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;                                    /* 0x1d */
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(be->sock[0], (struct sockaddr *)&addr, sizeof(addr)) < 0) {   /* 0x18 */
        close(be->sock[0]);
        free(be->sock);
        be->sock = NULL;
        return -1;
    }

    pthread_create(&be->rx_thread, NULL, vm_can_rx_thread, be);
    return 0;
}

/* OEM 0x158b60. */
void vm_can_tx(struct vm_can_backend *be, const struct vm_frame *f)
{
    struct vm_address a = { f->a0, f->a1, f->a2, f->a3 };
    struct can_frame cf;

    if (f->len >= 9)                              /* len<9 guard */
        return;

    memset(&cf, 0, sizeof(cf));
    cf.can_id = vm_can_id_encode(&a);
    cf.can_dlc = f->len;
    memcpy(cf.data, f->data, f->len);

    (void)write(be->sock[0], &cf, sizeof(cf));    /* expects 16 bytes */
}

/* OEM 0x158c30. */
void *vm_can_rx_thread(void *arg)
{
    struct vm_can_backend *be = arg;
    struct can_frame cf;
    struct vm_frame f;
    ssize_t n;

    while ((n = read(be->sock[0], &cf, sizeof(cf))) > 0) {
        if (cf.can_dlc < 9 && (cf.can_id & CAN_EFF_FLAG)) {   /* bit-31 / EFF */
            vm_can_id_decode(cf.can_id, &f);
            f.len = cf.can_dlc;
            memcpy(f.data, cf.data, cf.can_dlc);              /* up to 8 bytes */
            be->dispatch(be, &f);                             /* -> OD layer  */
        }
    }
    return NULL;
}
