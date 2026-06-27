/*
 * spi_mqtt_bridge.h — model for the VanMoof S5 i.MX8 `spi-mqtt-bridge`
 * (pkg vmxs5-embedded-spi-mqtt-bridge). Behaviour-oriented C.
 *
 * Run once per SPI satellite — `spi-mqtt-bridge ble` (nRF52840 @ /dev/spidev0.0)
 * and `spi-mqtt-bridge modem` (nRF9160 @ /dev/spidev0.1). It bridges the device's
 * protocol frames to/from the local MQTT bus: a GPIO "data-ready" interrupt wakes
 * a reader that pulls SPI frames carrying (connection_id, role, topic, payload),
 * turns them into MQTT publishes (and applies bridge/subscribe so the device can
 * ask for MQTT topics to be forwarded back to it over SPI). Each BLE connection
 * gets its own SPIMQTTClient; connection_id 0x80 = the "ble-ctrl" control client,
 * 0x81 = "modem-ctrl".
 *
 * The SPI transport-protocol framing (the spidev wrapper + the multi-byte frame
 * reassembly), the common::MQTTClient, std::thread/map/string and the GPIO/spidev
 * ioctls are VENDOR — modelled as opaque externs. OEM addresses are quoted in the
 * .c. Program "spi-mqtt-bridge", AArch64, image base 0x100000.
 */
#ifndef SPI_MQTT_BRIDGE_H
#define SPI_MQTT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void   common_logf(const char *file, int line, int level, const char *fmt, ...);
enum { LOG_DBG = 1, LOG_INF = 2, LOG_WRN = 3, LOG_ERR = 4 };

/* ---- per-device config (built by _INIT_1 @0x106840) -------------------- *
 * Two instances: "ble" and "modem". GPIO numbers are i.MX8 sysfs lines.     */
typedef struct spi_device_config {
    const char *name;        /* "ble" | "modem" (matched against argv[1]) */
    const char *spidev;      /* "/dev/spidev0.0" (ble) | "/dev/spidev0.1" (modem) */
    int         reset_gpio;  /* 83 (ble) | 84 (modem) — pulsed low/high to reset the nRF */
    int         ready_gpio;  /* 85 (ble) | 86 (modem) — data-ready / handshake line */
    int         aux_gpio;    /* 59 (ble) | 61 (modem) */
    const char *label;       /* "BLE" | "MODEM" (log tag) */
} spi_device_config;

extern const spi_device_config SPI_CFG_BLE;     /* DAT_00141068 */
extern const spi_device_config SPI_CFG_MODEM;   /* DAT_001410e8 */

#define SPI_BRIDGE_SPEED_HZ   6000000   /* 6 MHz SPI clock (set in main) */
#define SPI_RESET_SETTLE_US   20000     /* 20 ms reset-pulse half period */
#define SPI_FRAME_BUF         0x800     /* 2048-byte frame staging buffer */

/* ---- common framework: opaque handles (vendor) ------------------------- */
typedef struct mqtt_client mqtt_client; /* common::MQTTClient (mosquitto, localhost:1883) */
typedef struct spi_channel spi_channel; /* spidev wrapper + frame TP layer */

/* ---- a single SPI frame (connection_id, role, topic, payload) ---------- */
typedef struct spi_frame {
    uint8_t     connection_id;  /* BLE link id, or 0x80=ble-ctrl / 0x81=modem-ctrl */
    int         role;
    const char *topic;          /* the MQTT topic carried by the frame */
    const void *payload;
    size_t      len;
} spi_frame;

/* ---- per-connection MQTT client (RTTI 13SPIMQTTClient, 0x268 bytes) ----- */
typedef struct spi_mqtt_client spi_mqtt_client;     /* ctor FUN_00114260 */
spi_mqtt_client *spi_mqtt_client_new(mqtt_client *mqtt, uint8_t connection_id,
                                     int role, const char *topic_prefix);
void spi_mqtt_client_delete(spi_mqtt_client *c);
/* dynamic subscription: the device asks (over bridge/subscribe) for an MQTT
 * topic to be forwarded to it; "Already subscribed to %s" if duplicate. */
void spi_mqtt_client_subscribe(spi_mqtt_client *c, const char *topic);   /* bridge/subscribe */
void spi_mqtt_client_unsubscribe(spi_mqtt_client *c, const char *topic); /* bridge/unsubscribe */

/* ---- the client manager (client_manager.cpp) --------------------------- *
 * A hashmap of SPIMQTTClient keyed by connection_id (the bucket array is
 * @+0x08, bucket count @+0x10 in the OEM object). */
typedef struct client_manager client_manager;

/* get-or-create the client for (connection_id, role): keep it if the role
 * matches, else "Replace the current SPI-MQTT Client" (client_manager.cpp:0x25),
 * else "Create new SPI-MQTT Client" (0x15). 0x80 -> "ble-ctrl", 0x81 ->
 * "modem-ctrl"; any other id -> a per-connection prefix. (OEM FUN_00106e40) */
spi_mqtt_client *client_manager_get_or_create(client_manager *m,
                                              uint8_t connection_id, int role);
/* "BLE is restarted, clear all spi-mqtt-clients for the the pending app
 * connection" — drop every client on a BLE reset. */
void client_manager_clear_all(client_manager *m);

/* ---- the bridge -------------------------------------------------------- */
typedef struct spi_mqtt_bridge {
    const spi_device_config *cfg;
    mqtt_client             *mqtt;
    spi_channel             *spi;       /* +0xd0 region: spidev + 32 KB buffer */
    client_manager          *clients;   /* +0x08 bucket array */
    bool                     running;   /* +0x8130 */
} spi_mqtt_bridge;

/* reset the nRF over its GPIO: drive reset_gpio low, 20 ms, high, 20 ms. */
void spi_bridge_reset_device(const spi_device_config *cfg);              /* main */
/* open spidev, register the GPIO data-ready interrupt, spawn the reader thread.
 * Returns 0 on success; non-zero -> "Error initializing SPI channel"
 * (main.cpp:0x163). (OEM FUN_001150e0) */
int  spi_bridge_run(spi_mqtt_bridge *b, const spi_device_config *cfg, mqtt_client *mqtt);
/* the reader thread (FUN_00114c60): on data-ready, read SPI frames and route
 * each to its connection's client / MQTT; "Error writing to SPI channel %ld". */
void spi_bridge_on_frame(spi_mqtt_bridge *b, const spi_frame *f);

#endif /* SPI_MQTT_BRIDGE_H */
