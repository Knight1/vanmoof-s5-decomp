/*
 * main.c — VanMoof S5 i.MX8 `spi-can-if-linux` entry.
 *   OEM: devices/main/spi-can-bridge/src/main.cpp, program "spi-can-if-linux",
 *   AArch64, image base 0x100000, main @ 0x1065f0.
 *
 * Run as `spi-can-if-linux bridge`. Selects the named config (default "bridge"),
 * sets up the GPIO data-ready line, then brings up the bridge: vmlib, a composite
 * channel, the SocketCAN endpoint (vcan0), and the SPI-master endpoint to the
 * imx8_bridge, and starts the composite channel that couples them.
 *
 * The CLI/config + init ladder + error/log strings are reproduced from the
 * 0x1065f0 decompilation; the channels are reconstructed (SocketCAN) / modelled
 * (SPIM + CAN-TP) per spi_can_bridge.h.
 */
#include "spi_can_bridge.h"

#include <stdio.h>
#include <string.h>

#define MAIN_SRC "devices/main/spi-can-bridge/src/main.cpp"

/* vmlib bring-up (vendor): the common vm library init. */
extern bool vmlib_init(void);

int main(int argc, char **argv)
{
    const can_bridge_config *cfg = &CAN_CFG_BRIDGE;
    socketcan_channel       *can = NULL;
    spim_channel            *spi = NULL;
    composite_channel       *cc  = NULL;
    int                      rc  = 1;

    /* default config "bridge"; argv[1] may name an alternate. */
    if (argc > 1 && strcmp(argv[1], CAN_CFG_BRIDGE.name) != 0)
        cfg = &CAN_CFG_BRIDGE;   /* (only "bridge" is defined in this build) */

    /* GPIO data-ready line: init + toggle 0/1 (OEM FUN_001087f0 + set 0/1). */
    can_bridge_gpio_init(cfg->irq_pin);

    common_logf(MAIN_SRC, 0x62, LOG_WRN, "Starting on %s, irq_pin=%d",
                cfg->spidev, cfg->irq_pin);

    if (!vmlib_init()) {
        common_logf(MAIN_SRC, 0x65, LOG_ERR, "Could not initialize vmlib");
        return 1;
    }

    cc = composite_open();
    if (cc == NULL) {
        common_logf(MAIN_SRC, 0x6b, LOG_ERR, "Failed to initialize the composite channel");
        goto out;
    }

    can = socketcan_open(cfg->can_ifname);          /* "vcan0" */
    if (can == NULL) {
        common_logf(MAIN_SRC, 0x73, LOG_ERR, "Failed to initialize SocketCAN");
        goto out;
    }

    spi = spim_open(cfg);                            /* /dev/spidev1.0 */
    if (spi == NULL) {
        common_logf(MAIN_SRC, 0x7a, LOG_ERR, "Failed to initialize SPIM channel");
        goto out;
    }

    composite_attach(cc, can, spi);
    if (composite_start(cc) != 0) {                 /* the bridge loop (blocks) */
        common_logf(MAIN_SRC, 0x7f, LOG_ERR, "Failed to start composite channel");
        goto out;
    }
    rc = 0;

out:
    spim_close(spi);
    socketcan_close(can);
    composite_close(cc);
    return rc;
}
