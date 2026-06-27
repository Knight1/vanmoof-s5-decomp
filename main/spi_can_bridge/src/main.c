/*
 * main.c — VanMoof S5 i.MX8 `spi-can-if-linux` entry.
 *   OEM: devices/main/spi-can-bridge/src/main.cpp, program "spi-can-if-linux",
 *   AArch64, image base 0x100000, main @ 0x1065f0.
 *
 * Run as `spi-can-if-linux bridge`. Looks up the named config (default "bridge",
 * argv[1] overrides), pulses the GPIO data-ready line, traps SIGINT/SIGTERM/
 * SIGQUIT, then brings up the bridge: vmlib, a composite channel, a SocketCAN
 * endpoint (vcan0) on the first composite handle, and a SPI-master endpoint to
 * the imx8_bridge on the second handle; starts the composite, then blocks on a
 * semaphore until a signal arrives, and tears down.
 *
 * Reproduced from the 0x1065f0 decompilation (Sonnet-validated): the two config
 * lookups, the GPIO init+0+1 sequence, the signal handlers, the sem_init/
 * sem_wait join, the composite init + two get_handle calls, the method call
 * signatures, the success-only teardown order, and the literal log levels
 * (errors = 1, startup = 3).
 */
#include "spi_can_bridge.h"

#include <stdio.h>
#include <signal.h>
#include <semaphore.h>

#define MAIN_SRC "devices/main/spi-can-bridge/src/main.cpp"

static sem_t            g_done;        /* DAT_00138058 — released by the signal handler */
static composite_channel *g_composite; /* DAT_0013ab40 */
static vmlib            *g_vmlib;      /* DAT_00138318 */

/* the shared SIGINT/SIGTERM/SIGQUIT handler (DAT_001070b0): post the semaphore
 * so main() unblocks and tears the bridge down cleanly. */
static void sig_handler(int sig)
{
    (void)sig;
    sem_post(&g_done);
}

int main(int argc, char **argv)
{
    const can_bridge_config *cfg;
    gpio_line                gpio;
    channel_handle          *can_h;     /* DAT_001381c8 — SocketCAN endpoint */
    channel_handle          *spi_h;     /* DAT_001381d0 — SPI-master endpoint */
    const char              *ifname = CAN_IFNAME;   /* "vcan0" (a local literal) */

    /* config: default "bridge", then argv[1] overrides (two table lookups). */
    cfg = config_lookup_by_name("bridge");
    if (argc > 1)
        cfg = config_lookup_by_name(argv[1]);

    /* GPIO data-ready: init the line, drive it 0 then 1. */
    gpio_line_init(&gpio, cfg->gpio_line);   /* /sys/class/gpio/gpio7 */
    gpio_line_set(&gpio, 0);
    gpio_line_set(&gpio, 1);

    /* trap the shutdown signals. */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);

    sem_init(&g_done, 0, 0);

    common_logf(MAIN_SRC, 0x62, 3, "Starting on %s, irq_pin=%d",
                cfg->spidev, cfg->irq_pin);

    /* ---- init ladder (each failure logs at level 1 and returns 1) -------- */
    if (!vmlib_init(g_vmlib)) {
        common_logf(MAIN_SRC, 0x65, 1, "Could not initialize vmlib");
        return 1;
    }
    if (composite_init(g_composite) != 0) {
        common_logf(MAIN_SRC, 0x6b, 1, "Failed to initialize the composite channel");
        return 1;
    }
    can_h = composite_get_handle(g_composite);
    if (socketcan_open(can_h, &ifname) != 0) {       /* vcan0 */
        common_logf(MAIN_SRC, 0x73, 1, "Failed to initialize SocketCAN");
        return 1;
    }
    spi_h = composite_get_handle(g_composite);
    if (spim_channel_init(spi_h, cfg) != 0) {        /* /dev/spidev1.0 */
        common_logf(MAIN_SRC, 0x7a, 1, "Failed to initialize SPIM channel");
        return 1;
    }
    if (composite_start(g_composite) != 0) {
        common_logf(MAIN_SRC, 0x7f, 1, "Failed to start composite channel");
        return 1;
    }

    /* ---- run: block until a signal posts the semaphore, then tear down --- */
    sem_wait(&g_done);
    sem_destroy(&g_done);
    spim_channel_close(spi_h);
    socketcan_close(can_h);
    composite_stop(g_composite);
    vmlib_deinit(g_vmlib);
    return 0;
}
