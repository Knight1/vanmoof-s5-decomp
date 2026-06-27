/*
 * spi_can_bridge.h — model for the VanMoof S5 i.MX8 `spi-can-if-linux`
 * (pkg vmxs5-embedded-spi-can-bridge). Behaviour-oriented C.
 *
 * Run as `spi-can-if-linux bridge`. It exposes the bike's CAN fleet on a
 * SocketCAN interface (vcan0) by bridging it over SPI to the **imx8_bridge**
 * co-processor (a discrete Cortex-M on /dev/spidev1.0). A composite channel
 * couples a SocketCAN endpoint to a SPI-master endpoint, and a CAN
 * transport-protocol (TP) layer reassembles the 8-byte CAN frames into larger
 * multiframe transfers (the same lib/src/tp layer mqtt-ftp defers to).
 *
 * The SPI-master frame protocol + the CAN-TP reassembly, the GPIO data-ready
 * interrupt and the std::thread plumbing are VENDOR — modelled as opaque
 * externs. The SocketCAN endpoint (socket/ioctl/bind/can_frame) is reconstructed
 * faithfully. OEM addresses are quoted in the .c. Program "spi-can-if-linux",
 * AArch64, image base 0x100000.
 */
#ifndef SPI_CAN_BRIDGE_H
#define SPI_CAN_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void common_logf(const char *file, int line, int level, const char *fmt, ...);
enum { LOG_DBG = 1, LOG_INF = 2, LOG_WRN = 3, LOG_ERR = 4 };

/* ---- per-instance config (config table @0x138078, keyed by name) -------- *
 * The default instance is "bridge"; argv[1] can name an alternate. Each entry
 * carries a spidev path + a GPIO data-ready irq pin. (FUN_00107120 lookup.)   */
typedef struct can_bridge_config {
    const char *name;        /* "bridge" (matched against argv[1]) */
    const char *spidev;      /* "/dev/spidev1.0" — the imx8_bridge co-processor */
    int         irq_pin;     /* GPIO data-ready line from the imx8_bridge */
    const char *can_ifname;  /* "vcan0" */
} can_bridge_config;

extern const can_bridge_config CAN_CFG_BRIDGE;   /* DAT_00138078["bridge"] */

/* ---- SocketCAN endpoint (FUN_0010bca0) — reconstructed ----------------- */
typedef struct socketcan_channel socketcan_channel;
/* socket(PF_CAN, SOCK_RAW, CAN_RAW) + SIOCGIFINDEX("vcan0") + bind; spawns a
 * reader thread that forwards inbound CAN frames toward the SPI side.
 * Returns NULL on failure ("Failed to initialize SocketCAN" main.cpp:0x73). */
socketcan_channel *socketcan_open(const char *ifname);
int  socketcan_send(socketcan_channel *ch, uint32_t can_id, const void *data, uint8_t dlc);
void socketcan_close(socketcan_channel *ch);

/* ---- SPI-master endpoint (spim_channel.cpp, FUN_00107950) — modelled --- */
typedef struct spim_channel spim_channel;
spim_channel *spim_open(const can_bridge_config *cfg);    /* opens spidev, spawns SPI thread */
void          spim_close(spim_channel *ch);

/* ---- the composite bridge + CAN-TP multiframe (vendor lib/src/tp) ------- *
 * Couples the SocketCAN and SPI endpoints, reassembling multi-frame transfers
 * keyed by (src, idx, trgt). "Multiframe transfer too large (already received:
 * %zu, new DLC: %d, limit: %d)"; "Too many open multiframe write transfers.
 * Will drop an older transfer (src: %d, idx: %d, trgt: %d)". */
typedef struct composite_channel composite_channel;
composite_channel *composite_open(void);                  /* FUN_0010a720 */
int  composite_attach(composite_channel *cc, socketcan_channel *can, spim_channel *spi);
int  composite_start(composite_channel *cc);              /* FUN_0010a730 (run) */
void composite_close(composite_channel *cc);

/* ---- GPIO data-ready (gpio.cpp / gpio_polling.cpp) — modelled ---------- */
void can_bridge_gpio_init(int irq_pin);                   /* main: FUN_001087f0 + set 0/1 */

#endif /* SPI_CAN_BRIDGE_H */
