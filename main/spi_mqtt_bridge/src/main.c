/*
 * main.c — VanMoof S5 i.MX8 `spi-mqtt-bridge` entry.
 *   OEM: devices/main/spi-mqtt-bridge/src/main.cpp, program "spi-mqtt-bridge",
 *   AArch64, image base 0x100000, main @ 0x1065f0.
 *
 * Usage: spi-mqtt-bridge <ble/modem>
 * Selects the per-device config (ble -> /dev/spidev0.0 nRF52840; modem ->
 * /dev/spidev0.1 nRF9160), resets the nRF over its GPIO, configures SPI at
 * 6 MHz, and runs the bridge. An invalid device argument throws
 * std::invalid_argument("invalid device"); a failed channel logs
 * "Error initializing SPI channel" (main.cpp:0x163).
 */
#include "spi_mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

/* common ServiceEnv MQTT client (vendor): localhost:1883 keepalive 60. */
extern mqtt_client *bridge_mqtt_connect(const char *client_name);
extern void         bridge_mqtt_disconnect(mqtt_client *c);

int main(int argc, char **argv)
{
    const spi_device_config *cfg;
    spi_mqtt_bridge          bridge;
    mqtt_client             *mqtt;
    int                      rc;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <ble/modem>\n", argv[0]);
        return 0x16;                              /* EINVAL */
    }

    if (strcmp(argv[1], SPI_CFG_BLE.name) == 0)
        cfg = &SPI_CFG_BLE;
    else if (strcmp(argv[1], SPI_CFG_MODEM.name) == 0)
        cfg = &SPI_CFG_MODEM;
    else
        /* OEM throws std::invalid_argument("invalid device"). */
        { fprintf(stderr, "invalid device\n"); return 0x16; }

    /* reset the nRF: drive reset low, 20 ms, high, 20 ms. */
    spi_bridge_reset_device(cfg);

    mqtt = bridge_mqtt_connect(cfg->name);

    rc = spi_bridge_run(&bridge, cfg, mqtt);      /* opens spidev, runs reader */
    if (rc != 0)
        common_logf("devices/main/spi-mqtt-bridge/src/main.cpp", 0x163, LOG_DBG,
                    "Error initializing SPI channel");

    bridge_mqtt_disconnect(mqtt);
    return 0;
}
