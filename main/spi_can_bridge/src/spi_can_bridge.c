/*
 * spi_can_bridge.c — VanMoof S5 i.MX8 `spi-can-if-linux` core. Behaviour-oriented
 * C translation of the OEM AArch64 decompilation (program "spi-can-if-linux",
 * base 0x100000): the "bridge" config, the GPIO data-ready line, and the
 * SocketCAN endpoint (FUN_0010bca0). The SPI-master endpoint (spim_channel.cpp,
 * FUN_00107950) and the composite channel + CAN-TP multiframe reassembly are the
 * vendor lib/src/tp layer — modelled as opaque externs.
 *
 * The SocketCAN side is reconstructed faithfully: socket(PF_CAN, SOCK_RAW,
 * CAN_RAW), SIOCGIFINDEX("vcan0"), bind(sockaddr_can, 24). The kernel CAN ABI is
 * inlined so this is self-contained.
 */
#include "spi_can_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

/* ---- inlined kernel CAN ABI (linux/can.h) ------------------------------ */
#ifndef PF_CAN
#define PF_CAN 29
#endif
#ifndef AF_CAN
#define AF_CAN 29
#endif
#define CAN_RAW        1
#define SIOCGIFINDEX_  0x8933

struct can_frame_ {                  /* struct can_frame */
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  __pad[3];
    uint8_t  data[8];
};
struct sockaddr_can_ {               /* struct sockaddr_can (24 bytes, OEM bind len 0x18) */
    uint16_t can_family;
    int      can_ifindex;
    uint32_t rx_id;
    uint32_t tx_id;
    uint32_t __pad;
};

/* ======================================================================== *
 * "bridge" config (config table @0x138078; the imx8_bridge is on SPI1).
 * The spidev path is a runtime std::string in the config table; of the three
 * SPI devices (/dev/spidev0.0=ble, 0.1=modem, 1.0=?), the CAN bridge talks to
 * the imx8_bridge co-processor on /dev/spidev1.0.
 * ======================================================================== */
const can_bridge_config CAN_CFG_BRIDGE = {
    .name = "bridge", .spidev = "/dev/spidev1.0",
    .irq_pin = 0 /* GPIO data-ready (from the config table) */,
    .can_ifname = "vcan0",
};

/* ======================================================================== *
 * GPIO data-ready (modelled over sysfs) — OEM main: ctor + set 0/1.
 * ======================================================================== */
extern void bridge_sleep_us(long us);

static void gpio_write_file(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(val, fp); fclose(fp); }
}

void can_bridge_gpio_init(int irq_pin)
{
    char p[64], v[16];
    snprintf(v, sizeof v, "%d", irq_pin);
    gpio_write_file("/sys/class/gpio/export", v);
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", irq_pin);
    gpio_write_file(p, "out");                    /* DAT_00129630 "out" */
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", irq_pin);
    gpio_write_file(p, "0");
    gpio_write_file(p, "1");
}

/* ======================================================================== *
 * SocketCAN endpoint (OEM FUN_0010bca0) — reconstructed
 * ======================================================================== */
struct socketcan_channel { int fd; };

extern int socketcan_start_reader(socketcan_channel *ch);   /* thread FUN_0010bba0 (vendor) */

socketcan_channel *socketcan_open(const char *ifname)
{
    socketcan_channel  *ch = (socketcan_channel *)calloc(1, sizeof *ch);
    struct ifreq        ifr;
    struct sockaddr_can_ addr;

    if (!ch)
        return NULL;

    /* socket(PF_CAN, SOCK_RAW, CAN_RAW) — OEM FUN_001058d0(0x1d, 3, 1). */
    ch->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (ch->fd < 0) {
        free(ch);
        return NULL;
    }

    /* SIOCGIFINDEX on the interface name ("vcan0" by default). */
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", (ifname && ifname[0]) ? ifname : "vcan0");
    if (ioctl(ch->fd, SIOCGIFINDEX_, &ifr) < 0) {
        close(ch->fd);
        free(ch);
        return NULL;
    }

    /* bind to the CAN interface (OEM bind len 0x18 = 24). */
    memset(&addr, 0, sizeof addr);
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(ch->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(ch->fd);
        free(ch);
        return NULL;
    }

    /* spawn the reader thread that forwards inbound CAN frames to the SPI side. */
    socketcan_start_reader(ch);
    return ch;
}

int socketcan_send(socketcan_channel *ch, uint32_t can_id, const void *data, uint8_t dlc)
{
    struct can_frame_ frame;
    memset(&frame, 0, sizeof frame);
    frame.can_id  = can_id;
    frame.can_dlc = dlc > 8 ? 8 : dlc;
    memcpy(frame.data, data, frame.can_dlc);
    return (write(ch->fd, &frame, sizeof frame) == (long)sizeof frame) ? 0 : -1;
}

void socketcan_close(socketcan_channel *ch)
{
    if (ch) {
        if (ch->fd >= 0)
            close(ch->fd);
        free(ch);
    }
}

/* ======================================================================== *
 * SPI-master endpoint + composite channel + CAN-TP (vendor, modelled)
 *
 * The SPIM channel opens /dev/spidev1.0 and runs the SPI frame protocol with the
 * imx8_bridge over a GPIO data-ready handshake. The composite channel couples
 * the SocketCAN and SPIM endpoints; the CAN transport-protocol (TP) layer
 * reassembles multi-frame transfers keyed by (src, idx, trgt):
 *   "Multiframe transfer too large (already received: %zu, new DLC: %d, limit: %d)"
 *   "Too many open multiframe write transfers. Will drop an older transfer
 *    (src: %d, idx: %d, trgt: %d)"
 * These bodies live in lib/src/tp + spim_channel.cpp and are not reconstructed
 * here; the bring-up wiring is modelled so the architecture reads end-to-end.
 * ======================================================================== */
extern spim_channel      *spim_channel_create(const can_bridge_config *cfg);
extern int                spim_channel_start(spim_channel *ch);
extern composite_channel *composite_channel_create(void);
extern int                composite_channel_couple(composite_channel *cc,
                                                   socketcan_channel *can,
                                                   spim_channel *spi);
extern int                composite_channel_run(composite_channel *cc);
extern void               composite_channel_destroy(composite_channel *cc);
extern void               spim_channel_destroy(spim_channel *ch);

spim_channel *spim_open(const can_bridge_config *cfg)
{
    spim_channel *ch = spim_channel_create(cfg);  /* opens spidev, spawns SPI thread */
    if (ch == NULL)
        return NULL;
    if (spim_channel_start(ch) != 0) {
        spim_channel_destroy(ch);
        return NULL;
    }
    return ch;
}

void spim_close(spim_channel *ch) { spim_channel_destroy(ch); }

composite_channel *composite_open(void) { return composite_channel_create(); }

int composite_attach(composite_channel *cc, socketcan_channel *can, spim_channel *spi)
{
    return composite_channel_couple(cc, can, spi);
}

int composite_start(composite_channel *cc) { return composite_channel_run(cc); }

void composite_close(composite_channel *cc) { composite_channel_destroy(cc); }
