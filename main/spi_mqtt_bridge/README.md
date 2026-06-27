# spi_mqtt_bridge — SPI ↔ MQTT bridge (i.MX8)

`/usr/bin/spi-mqtt-bridge` — the VanMoof S5 **SPI ↔ MQTT bridge**, package
`vmxs5-embedded-spi-mqtt-bridge`. Stripped AArch64 C++ ELF (`AARCH64:LE:64:v8A`,
base `0x100000`, ~399 functions), Ghidra `/S5-v1.5/OS/spi-mqtt-bridge`. Run once
per SPI satellite by systemd: `spi-mqtt-bridge@ble.service`
(`spi-mqtt-bridge ble`) and `spi-mqtt-bridge@modem.service`.

## What it does

It puts the **nRF52840 (ble)** and **nRF9160 (modem)** SPI satellites onto the
local MQTT bus — it is the source/sink of the whole `ble/*` and `modem/*`
namespace.

- `spi-mqtt-bridge ble` → `/dev/spidev0.0` (CS0), `modem` → `/dev/spidev0.1`
  (CS1). An invalid argument throws `std::invalid_argument("invalid device")`.
- On start it **resets the nRF** over its GPIO (drive reset low, 20 ms, high,
  20 ms — ble reset GPIO **83**, modem **84**), then opens spidev at **6 MHz**
  and registers the nRF's **GPIO data-ready interrupt** (ble **85**, modem
  **86**). A failure logs `"Error initializing SPI channel"` (main.cpp:0x163).
- A reader thread waits on data-ready, reads SPI **frames** carrying
  `(connection_id, role, topic, payload)`, and bridges each to MQTT.
- Each BLE connection gets its own **`SPIMQTTClient`**, keyed by `connection_id`
  (a hashmap in `client_manager.cpp`): the role is matched, else the client is
  *replaced*, else *created*. **`connection_id 0x80` = `ble-ctrl`**, **`0x81` =
  `modem-ctrl`** (the broker ACL usernames). A BLE restart clears all clients.
- The device drives its own forwarding: it asks the bridge to subscribe MQTT
  topics on its behalf via **`bridge/subscribe`** / **`bridge/unsubscribe`**
  (the matched MQTT messages are then pushed back over SPI); duplicates log
  `"Already subscribed to %s"`.

Broker is the shared `common::MQTTClient` to **`localhost:1883`** (keepalive 60).

## Scope

VanMoof-authored logic — the CLI + config ([`src/main.c`](src/main.c)), the GPIO
reset, the SPI channel bring-up, and the per-connection client manager
([`src/spi_mqtt_bridge.c`](src/spi_mqtt_bridge.c)) — is reconstructed. The
**spidev SPI-transport-protocol frame layer**, the GPIO data-ready interrupt,
the `std::thread` reader, `common::MQTTClient` and `std::map`/`string` are vendor
— modelled as opaque externs. (The deep per-byte SPI frame wire format lives in
that transport layer; the bridge-level routing + client model are reconstructed.)

- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/spi-mqtt-bridge_program.json`](ghidra/exports/spi-mqtt-bridge_program.json)
- **MQTT topics:** [`../docs/mqtt-topics.md`](../docs/mqtt-topics.md) (the `ble/*` + `modem/*` namespaces + `bridge/*`)

## Build

```sh
make            # src/{main,spi_mqtt_bridge}.c -> build/*.o, clean -Wall -Wextra -Wpedantic
```
