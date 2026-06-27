/*
 * spi_can_bridge.c — VanMoof S5 i.MX8 `spi-can-if-linux` core. Behaviour-oriented
 * C translation of the OEM AArch64 decompilation (program "spi-can-if-linux",
 * base 0x100000), Sonnet-validated: the config table (_INIT_1 @0x1069e0), the
 * name lookup (FUN_00107120), the GPIO line, and the SocketCAN endpoint
 * (socketcan_open 0x10bca0). The SPI-master endpoint, the composite channel +
 * CAN-TP multiframe, vmlib and the std::thread plumbing are VENDOR — opaque
 * externs.
 */
#include "spi_can_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
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
#define CAN_RAW_       1
#define SIOCGIFINDEX_  0x8933

struct can_frame_ { uint32_t can_id; uint8_t can_dlc; uint8_t __pad[3]; uint8_t data[8]; };
struct sockaddr_can_ { uint16_t can_family; int can_ifindex; uint32_t rx_id, tx_id, __pad; };

/* ======================================================================== *
 * config table (OEM _INIT_1 @0x1069e0; 3 × 0x70-byte entries @0x138078)
 * "bridge" values resolved by the Sonnet validation pass.
 * ======================================================================== */
const can_bridge_config CAN_CFG_BLE = {
    .name = "ble",   .spidev = "/dev/spidev0.0", .irq_pin = 0, .gpio_line = 0,
    .field_0x48 = 0, .canif_input = "/dev/input/spi0.0",
};
const can_bridge_config CAN_CFG_MODEM = {
    .name = "modem", .spidev = "/dev/spidev0.1", .irq_pin = 0, .gpio_line = 0,
    .field_0x48 = 0, .canif_input = "/dev/input/spi0.1",
};
const can_bridge_config CAN_CFG_BRIDGE = {
    .name = "bridge", .spidev = "/dev/spidev1.0",
    .irq_pin = 8,        /* entry+0x40 lo32 — logged as "irq_pin=%d" */
    .gpio_line = 7,      /* entry+0x44 — /sys/class/gpio/gpio7 */
    .field_0x48 = 65,    /* entry+0x48 (0x41) */
    .canif_input = "/dev/input/spi1.0",   /* entry+0x50 */
};

static const can_bridge_config *const k_config_table[] = {
    &CAN_CFG_BLE, &CAN_CFG_MODEM, &CAN_CFG_BRIDGE,
};

const can_bridge_config *config_lookup_by_name(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_config_table / sizeof k_config_table[0]; i++)
        if (strcmp(k_config_table[i]->name, name) == 0)
            return k_config_table[i];
    /* OEM throws std::invalid_argument("invalid device") (uncaught -> abort). */
    fprintf(stderr, "invalid device\n");
    exit(1);
}

/* ======================================================================== *
 * GPIO line (OEM gpio_line_init 0x1087f0 / gpio_line_set 0x108a10, sysfs)
 * ======================================================================== */
static void gpio_write_file(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(val, fp); fclose(fp); }
}

void gpio_line_init(gpio_line *g, int pin)
{
    char p[64], v[16];
    g->pin = pin;
    snprintf(v, sizeof v, "%d", pin);
    gpio_write_file("/sys/class/gpio/export", v);
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", pin);
    gpio_write_file(p, "out");                    /* DAT_00129630 "out" */
}

void gpio_line_set(gpio_line *g, int value)
{
    char p[64];
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", g->pin);
    gpio_write_file(p, value ? "1" : "0");
}

/* ======================================================================== *
 * composite sub-channel handle (the +0x20 send-fn / +0x28 endpoint object)
 * ======================================================================== */
struct channel_handle {
    int (*send_fn)(channel_handle *, uint32_t, const void *, uint8_t); /* +0x20 */
    void *inner;       /* +0x28: the endpoint state (CAN fd-holder / SPIM block) */
};

/* the inner CAN fd-holder (OEM calloc(1, 0x10)): fd @0, pthread_t @8. */
struct can_inner { int fd; int _pad; pthread_t tid; };

extern void *socketcan_reader_thread(void *arg);  /* OEM 0x10bba0 (vendor) */

/* ======================================================================== *
 * SocketCAN endpoint — OEM socketcan_open 0x10bca0 (reconstructed)
 * A METHOD on a composite handle: store the send fn, attach a CAN socket bound
 * to the interface, and spawn the reader thread. Returns 0 / -1.
 * ======================================================================== */
int socketcan_open(channel_handle *self, const char **ifname)
{
    struct can_inner *in;
    struct ifreq      ifr;
    struct sockaddr_can_ addr;
    const char       *name = (ifname && *ifname && (*ifname)[0]) ? *ifname : "vcan0";

    self->send_fn = socketcan_send;              /* +0x20, first action */

    in = (struct can_inner *)calloc(1, sizeof *in);   /* 16-byte inner, +0x28 */
    if (!in)
        return -1;
    self->inner = in;

    /* socket(PF_CAN, SOCK_RAW, CAN_RAW) — OEM FUN_001058d0(0x1d, 3, 1). */
    in->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW_);
    if (in->fd < 0) {
        free(in);
        self->inner = NULL;
        return -1;
    }

    /* __strcpy_chk(ifr.ifr_name, name, 16); ioctl(SIOCGIFINDEX) — NOT checked. */
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, name, IFNAMSIZ);
    ioctl(in->fd, SIOCGIFINDEX_, &ifr);          /* OEM ignores the return value */

    /* bind to the CAN interface (OEM bind len 0x18 = 24). */
    memset(&addr, 0, sizeof addr);
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(in->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(in->fd);
        free(in);
        self->inner = NULL;
        return -1;
    }

    /* reader thread: forwards inbound CAN frames toward the SPI side; arg = self. */
    pthread_create(&in->tid, NULL, socketcan_reader_thread, self);
    return 0;
}

int socketcan_send(channel_handle *self, uint32_t can_id, const void *data, uint8_t dlc)
{
    struct can_inner *in = (struct can_inner *)self->inner;
    struct can_frame_ frame;
    memset(&frame, 0, sizeof frame);
    frame.can_id  = can_id;
    frame.can_dlc = dlc > 8 ? 8 : dlc;
    memcpy(frame.data, data, frame.can_dlc);
    return (write(in->fd, &frame, sizeof frame) == (long)sizeof frame) ? 0 : -1;
}

void socketcan_close(channel_handle *self)
{
    struct can_inner *in;
    if (!self)
        return;
    in = (struct can_inner *)self->inner;
    if (in) {
        if (in->fd >= 0)
            close(in->fd);
        free(in);
        self->inner = NULL;
    }
}

/* ======================================================================== *
 * SPI-master endpoint + composite channel + vmlib (vendor, modelled)
 *
 * spim_channel_init (OEM 0x107950): operator new(0xa8), a std::condition_variable
 * @+0x70, an internal ring-queue FUN_0011deb0(100 capacity, 13-byte elems) @+0x38,
 * the SPI reader thread (spim_reader_thread 0x107640). Returns 0 / -1 (queue) /
 * -2 (null). Returns 0 even if the reader later fails its SPI init ("Failed to
 * initialize SPI interface", spim_channel.cpp:0x3b). The composite channel
 * couples the two endpoints and runs the CAN-TP multiframe reassembly ("Multiframe
 * transfer too large …", "Too many open multiframe write transfers …"). These
 * bodies live in spim_channel.cpp + lib/src/tp and are modelled, not reconstructed.
 * ======================================================================== */
extern int  spim_channel_init(channel_handle *self, const can_bridge_config *cfg); /* 0x107950 */
extern void spim_channel_close(channel_handle *self);                              /* 0x107be0 */
extern int  composite_init(composite_channel *cc);                                 /* 0x10a600 */
extern channel_handle *composite_get_handle(composite_channel *cc);                /* 0x10a720 */
extern int  composite_start(composite_channel *cc);                                /* 0x10a730 */
extern void composite_stop(composite_channel *cc);                                 /* 0x10a800 */
extern bool vmlib_init(vmlib *v);                                                  /* 0x10ac60 */
extern void vmlib_deinit(vmlib *v);                                                /* 0x10ad00 */
