/*
 * spi_mqtt_bridge.c — VanMoof S5 i.MX8 `spi-mqtt-bridge` core. Behaviour-oriented
 * C translation of the OEM AArch64 decompilation (program "spi-mqtt-bridge",
 * base 0x100000): the per-device config (_INIT_1 @0x106840), the GPIO reset, the
 * SPI channel bring-up (FUN_001150e0), and the per-connection client manager
 * (client_manager.cpp, FUN_00106e40).
 *
 * The spidev wrapper + the SPI frame transport-protocol reassembly
 * (FUN_00123fe0), the GPIO data-ready interrupt (FUN_0011a080), the reader
 * thread (FUN_00114c60), common::MQTTClient and std::thread/map are VENDOR —
 * modelled as opaque externs / a simple table. The device configs, the GPIO
 * reset pulse, the connection_id/role keying, the ble-ctrl/modem-ctrl control
 * clients, the dynamic bridge/subscribe and the log strings are reproduced.
 */
#include "spi_mqtt_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CM_SRC "devices/main/spi-mqtt-bridge/src/client_manager.cpp"

/* ======================================================================== *
 * per-device config (OEM _INIT_1 @0x106840)
 * ======================================================================== */
const spi_device_config SPI_CFG_BLE = {
    .name = "ble", .spidev = "/dev/spidev0.0",
    .reset_gpio = 83, .ready_gpio = 85, .aux_gpio = 59, .label = "BLE",
};
const spi_device_config SPI_CFG_MODEM = {
    .name = "modem", .spidev = "/dev/spidev0.1",
    .reset_gpio = 84, .ready_gpio = 86, .aux_gpio = 61, .label = "MODEM",
};

/* ======================================================================== *
 * GPIO reset pulse (sysfs) — main pulses the reset line low then high.
 * ======================================================================== */
extern void bridge_sleep_us(long us);            /* nanosleep (vendor) */

static void gpio_write_file(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(val, fp); fclose(fp); }
}

static void gpio_export_output(int pin)
{
    char p[64], v[16];
    snprintf(v, sizeof v, "%d", pin);
    gpio_write_file("/sys/class/gpio/export", v);
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", pin);
    gpio_write_file(p, "out");
}

static void gpio_set(int pin, int value)
{
    char p[64];
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", pin);
    gpio_write_file(p, value ? "1" : "0");
}

void spi_bridge_reset_device(const spi_device_config *cfg)
{
    gpio_export_output(cfg->reset_gpio);
    gpio_set(cfg->reset_gpio, 0);            /* assert reset */
    bridge_sleep_us(SPI_RESET_SETTLE_US);    /* 20 ms */
    gpio_set(cfg->reset_gpio, 1);            /* release */
    bridge_sleep_us(SPI_RESET_SETTLE_US);    /* 20 ms */
}

/* ======================================================================== *
 * SPI channel (OEM FUN_001150e0) — modelled
 * ======================================================================== */
extern spi_channel *spi_channel_open(const char *spidev, uint32_t speed_hz);     /* spidev + ioctls */
extern int  spi_channel_register_ready_irq(spi_channel *ch, int ready_gpio);     /* GPIO edge */
extern int  spi_channel_start_reader(spi_channel *ch, spi_mqtt_bridge *b);        /* thread FUN_00114c60 */
extern int  spi_channel_write(spi_channel *ch, const spi_frame *f);              /* outbound frame */

int spi_bridge_run(spi_mqtt_bridge *b, const spi_device_config *cfg, mqtt_client *mqtt)
{
    memset(b, 0, sizeof *b);
    b->cfg  = cfg;
    b->mqtt = mqtt;

    b->spi = spi_channel_open(cfg->spidev, SPI_BRIDGE_SPEED_HZ);  /* 6 MHz */
    if (b->spi == NULL)
        return -1;                                /* "Error initializing SPI channel" */
    if (spi_channel_register_ready_irq(b->spi, cfg->ready_gpio) != 0)
        return -1;
    b->running = true;
    if (spi_channel_start_reader(b->spi, b) != 0) {
        b->running = false;
        return -1;
    }
    return 0;
}

/* ======================================================================== *
 * client manager (OEM client_manager.cpp / FUN_00106e40)
 * A hashmap of SPIMQTTClient keyed by connection_id. Modelled as a table.
 * ======================================================================== */
#define CM_MAX 64

struct client_manager {
    mqtt_client *mqtt;
    struct { uint8_t id; int role; spi_mqtt_client *cli; bool used; } slot[CM_MAX];
};

/* per-connection topic prefix: 0x80 -> "ble-ctrl", 0x81 -> "modem-ctrl",
 * else a per-connection prefix derived from the connection_id + role. */
static void client_prefix(uint8_t connection_id, int role, char *out, size_t n)
{
    if (connection_id == 0x80)
        snprintf(out, n, "ble-ctrl");
    else if (connection_id == 0x81)
        snprintf(out, n, "modem-ctrl");
    else
        snprintf(out, n, "%d-%d", connection_id, role);
}

spi_mqtt_client *client_manager_get_or_create(client_manager *m,
                                              uint8_t connection_id, int role)
{
    int free_slot = -1;
    int i;
    char prefix[32];

    for (i = 0; i < CM_MAX; i++) {
        if (m->slot[i].used && m->slot[i].id == connection_id) {
            if (m->slot[i].role == role)
                return m->slot[i].cli;            /* keep the existing client */
            /* role changed -> replace */
            common_logf(CM_SRC, 0x25, LOG_WRN,
                        "Replace the current SPI-MQTT Client, connection_id:%d, role:%d",
                        connection_id, role);
            spi_mqtt_client_delete(m->slot[i].cli);
            client_prefix(connection_id, role, prefix, sizeof prefix);
            m->slot[i].role = role;
            m->slot[i].cli  = spi_mqtt_client_new(m->mqtt, connection_id, role, prefix);
            return m->slot[i].cli;
        }
        if (!m->slot[i].used && free_slot < 0)
            free_slot = i;
    }

    /* not found -> create */
    common_logf(CM_SRC, 0x15, LOG_WRN,
                "Create new SPI-MQTT Client, connection_id:%d, role:%d",
                connection_id, role);
    if (free_slot < 0)
        return NULL;                              /* table full */
    client_prefix(connection_id, role, prefix, sizeof prefix);
    m->slot[free_slot].used = true;
    m->slot[free_slot].id   = connection_id;
    m->slot[free_slot].role = role;
    m->slot[free_slot].cli  = spi_mqtt_client_new(m->mqtt, connection_id, role, prefix);
    return m->slot[free_slot].cli;
}

void client_manager_clear_all(client_manager *m)
{
    int i;
    common_logf(CM_SRC, 0x00, LOG_WRN,
                "BLE is restarted, clear all spi-mqtt-clients for the the pending app connection");
    for (i = 0; i < CM_MAX; i++) {
        if (m->slot[i].used) {
            spi_mqtt_client_delete(m->slot[i].cli);
            m->slot[i].used = false;
            m->slot[i].cli  = NULL;
        }
    }
}

/* ======================================================================== *
 * per-connection SPI-MQTT client (RTTI 13SPIMQTTClient, OEM ctor FUN_00114260)
 * Holds the connection's MQTT client + its set of forwarded subscriptions.
 * ======================================================================== */
#define SC_MAX_SUBS 32

struct spi_mqtt_client {
    mqtt_client *mqtt;
    uint8_t      connection_id;
    int          role;
    char         prefix[32];
    char        *subs[SC_MAX_SUBS];
    int          nsubs;
};

extern void bridge_mqtt_subscribe(mqtt_client *c, const char *topic,
                                  spi_mqtt_client *owner);
extern void bridge_mqtt_unsubscribe(mqtt_client *c, const char *topic);
extern void bridge_mqtt_publish_frame(mqtt_client *c, const spi_frame *f);

spi_mqtt_client *spi_mqtt_client_new(mqtt_client *mqtt, uint8_t connection_id,
                                     int role, const char *topic_prefix)
{
    spi_mqtt_client *c = (spi_mqtt_client *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->mqtt = mqtt;
    c->connection_id = connection_id;
    c->role = role;
    snprintf(c->prefix, sizeof c->prefix, "%s", topic_prefix ? topic_prefix : "");
    return c;
}

void spi_mqtt_client_delete(spi_mqtt_client *c)
{
    int i;
    if (!c)
        return;
    for (i = 0; i < c->nsubs; i++) {
        bridge_mqtt_unsubscribe(c->mqtt, c->subs[i]);
        free(c->subs[i]);
    }
    free(c);
}

/* the device asked (over bridge/subscribe) for an MQTT topic to be forwarded. */
void spi_mqtt_client_subscribe(spi_mqtt_client *c, const char *topic)
{
    int i;
    for (i = 0; i < c->nsubs; i++)
        if (strcmp(c->subs[i], topic) == 0) {
            common_logf("devices/main/spi-mqtt-bridge/src/spi_mqtt_client.cpp", 0,
                        LOG_DBG, "Already subscribed to %s", topic);
            return;
        }
    if (c->nsubs < SC_MAX_SUBS) {
        c->subs[c->nsubs] = strdup(topic);
        bridge_mqtt_subscribe(c->mqtt, topic, c);
        c->nsubs++;
    }
}

void spi_mqtt_client_unsubscribe(spi_mqtt_client *c, const char *topic)
{
    int i;
    for (i = 0; i < c->nsubs; i++)
        if (strcmp(c->subs[i], topic) == 0) {
            bridge_mqtt_unsubscribe(c->mqtt, topic);
            free(c->subs[i]);
            c->subs[i] = c->subs[--c->nsubs];
            return;
        }
}

/* ======================================================================== *
 * frame routing (OEM reader thread FUN_00114c60)
 * An inbound SPI frame is routed to its connection's client. The "bridge/..."
 * control topics manage the dynamic subscriptions; everything else is
 * published to the bus under the device namespace.
 * ======================================================================== */
void spi_bridge_on_frame(spi_mqtt_bridge *b, const spi_frame *f)
{
    spi_mqtt_client *c = client_manager_get_or_create(b->clients, f->connection_id, f->role);
    if (c == NULL) {
        common_logf("devices/main/spi-mqtt-bridge/src/main.cpp", 0, LOG_WRN,
                    "No spi-mqtt client");
        return;
    }

    if (f->topic != NULL && strcmp(f->topic, "bridge/subscribe") == 0)
        spi_mqtt_client_subscribe(c, (const char *)f->payload);
    else if (f->topic != NULL && strcmp(f->topic, "bridge/unsubscribe") == 0)
        spi_mqtt_client_unsubscribe(c, (const char *)f->payload);
    else
        /* publish the device frame to the bus (a ble/... or modem/... topic). */
        bridge_mqtt_publish_frame(b->mqtt, f);
}
