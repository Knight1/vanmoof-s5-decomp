/*
 * spi_can_bridge.h — model for the VanMoof S5 i.MX8 `spi-can-if-linux`
 * (pkg vmxs5-embedded-spi-can-bridge). Behaviour-oriented C.
 *
 * Run as `spi-can-if-linux bridge`. It exposes the bike's CAN fleet on a
 * SocketCAN interface (vcan0) by bridging it over SPI to the **imx8_bridge**
 * co-processor (a discrete Cortex-M on /dev/spidev1.0; data-ready over
 * /dev/input/spi1.0). A composite channel couples a SocketCAN endpoint to a
 * SPI-master endpoint, and a CAN transport-protocol (TP) layer reassembles the
 * 8-byte CAN frames into larger multiframe transfers.
 *
 * Validated against the OEM (program "spi-can-if-linux", base 0x100000) with a
 * Sonnet decompile pass: the config table, the composite-handle wiring, the
 * SocketCAN method, and the GPIO/signal/sem bring-up are corrected to match.
 *
 * The SPI-master frame protocol + the CAN-TP reassembly, the std::thread
 * plumbing and the C++ framework objects are VENDOR — modelled as opaque
 * externs. The SocketCAN endpoint (socket/ioctl/bind/can_frame) is reconstructed
 * faithfully.
 */
#ifndef SPI_CAN_BRIDGE_H
#define SPI_CAN_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void common_logf(const char *file, int line, int level, const char *fmt, ...);
/* NB: in spi-can-if-linux the error logs use level 1 and the startup info uses
 * level 3 — those literals are reproduced verbatim at the call sites. */

/* ---- per-instance config (table @0x138078, 3 × 0x70-byte entries, built by
 * _INIT_1 @0x1069e0; keyed by name). Entry 0 "ble" (/dev/spidev0.0), entry 1
 * "modem" (/dev/spidev0.1), entry 2 "bridge" @0x138158 (/dev/spidev1.0). ----- */
typedef struct can_bridge_config {
    const char *name;        /* "bridge"            (entry+0x00, std::string) */
    const char *spidev;      /* "/dev/spidev1.0"    (entry+0x20, std::string) */
    uint32_t    irq_pin;     /* 8  — logged as "irq_pin=%d" in main (entry+0x40 lo32) */
    uint32_t    gpio_line;   /* 7  — /sys/class/gpio/gpio7 (entry+0x44) */
    uint32_t    field_0x48;  /* 65 (0x41) — entry+0x48, purpose TBD */
    const char *canif_input; /* "/dev/input/spi1.0" (entry+0x50, std::string) */
} can_bridge_config;

extern const can_bridge_config CAN_CFG_BLE;     /* entry 0 @0x138078 */
extern const can_bridge_config CAN_CFG_MODEM;   /* entry 1 @0x1380e8 */
extern const can_bridge_config CAN_CFG_BRIDGE;  /* entry 2 @0x138158 (default) */
/* table scan by name (FUN_00107120); throws std::invalid_argument("invalid
 * device") on miss. Returns the matching entry. */
const can_bridge_config *config_lookup_by_name(const char *name);

#define CAN_IFNAME "vcan0"   /* hardcoded local in main, NOT from the config table */

/* ---- composite channel (the manager that couples SocketCAN <-> SPIM) ----- *
 * composite_init() inits the registry; composite_get_handle() hands out a
 * sub-channel handle that an endpoint attaches to. (OEM: init FUN_0010a600,
 * get_handle FUN_0010a720, start FUN_0010a730, stop/free FUN_0010a800.) */
typedef struct composite_channel composite_channel; /* &DAT_0013ab40 in the OEM */
typedef struct channel_handle    channel_handle;
int             composite_init(composite_channel *cc);          /* 0x0010a600 */
channel_handle *composite_get_handle(composite_channel *cc);    /* 0x0010a720 */
int             composite_start(composite_channel *cc);         /* 0x0010a730 (run) */
void            composite_stop(composite_channel *cc);          /* 0x0010a800 */

/* ---- SocketCAN endpoint (OEM socketcan_open 0x10bca0) — a METHOD that attaches
 * a CAN socket to a composite handle; returns 0/-1. ------------------------- */
int  socketcan_open(channel_handle *self, const char **ifname);  /* 0x10bca0 */
int  socketcan_send(channel_handle *self, uint32_t can_id, const void *data, uint8_t dlc); /* 0x10bad0 (vtable +0x20) */
void socketcan_close(channel_handle *self);                      /* 0x10be00 */

/* ---- SPI-master endpoint (OEM spim_channel_init 0x107950) — int return ---- *
 * Attaches the SPI master to a composite handle; opaque (vendor): operator
 * new(0xa8), a std::condition_variable @+0x70, an internal ring-queue
 * (FUN_0011deb0(100 capacity, 13-byte elems) @+0x38), and the SPI reader thread
 * (spim_reader_thread 0x107640). Returns 0 ok / -1 queue-alloc-fail / -2 null.
 * NB: returns 0 even if the reader thread later fails its SPI init ("Failed to
 * initialize SPI interface", spim_channel.cpp:0x3b). */
int  spim_channel_init(channel_handle *self, const can_bridge_config *cfg);  /* 0x107950 */
void spim_channel_close(channel_handle *self);                   /* 0x107be0 */

/* ---- GPIO data-ready (gpio.cpp, sysfs) ---------------------------------- *
 * OEM uses a stack GPIO object: init(pin) then set value 0 then 1.           */
typedef struct gpio_line { int pin; } gpio_line;
void gpio_line_init(gpio_line *g, int pin);     /* 0x1087f0 (export + direction out) */
void gpio_line_set(gpio_line *g, int value);    /* 0x108a10 (write /value) */

/* ---- vmlib bring-up (vendor) -------------------------------------------- */
typedef struct vmlib vmlib;
bool vmlib_init(vmlib *v);                        /* 0x10ac60 */
void vmlib_deinit(vmlib *v);                      /* 0x10ad00 */

#endif /* SPI_CAN_BRIDGE_H */
