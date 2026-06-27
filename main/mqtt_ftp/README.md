# mqtt_ftp — file transfer over MQTT (i.MX8)

`/usr/bin/mqtt-ftp-service` — the VanMoof S5 **file-transfer-over-MQTT** server,
package `vmxs5-embedded-mqtt-ftp` (systemd `mqtt-ftp.service`, started as
`mqtt-ftp-service /tmp`). Stripped AArch64 C++ ELF (`AARCH64:LE:64:v8A`, base
`0x100000`, ~738 functions), Ghidra `/S5-v1.5/OS/mqtt-ftp-service`.

The package also ships **`mqtt_ftp_send`** — a small **factory firmware-update
sender** (despite the name, not a generic file pusher):
`mqtt_ftp_send ble/modem/imx8/phone test.bin <silent>` connects as client-id
`mqtt_update_client` / user `factory` and drives the `update` service's
**MqttUpdateClient** (reconstructed under [`../update/`](../update/)) to flash the
named target over the `ftp_server` channel — `"Failed performing firmware
update"` on error, or `"Target <x> does not have an implementation for requesting
version-information"`. It reuses already-reconstructed `update` code, so it has
no separate reconstruction here (`main` @0x1061b0, named in Ghidra).

## What it does

`MqttFtpService(IMQTTClient, fs::path dest, IClock, chunk_size)` push/pulls files
over MQTT for the fleet (log/cert/config transfer), rooted at a mandatory
**destination folder** (the unit passes `/tmp`):

- Subscribes **`ftp_server/command`** (JSON commands), publishes results on
  **`ftp_server/reply`** and a completion event on **`ftp_server/file_finished`**.
- A command with a `data` payload (or `flush:true`) is a **write**: the chunk is
  CRC32-checked against the command's `crc`, appended under `dest/<name>`
  (truncating on `index==0`), and acked; `flush` finalises and fires
  `file_finished`. A command without `data` is a **read**: the requested chunk
  (`index × chunk_size`, default **512**) is read back, CRC32'd, and returned.
- A `silent:true` request suppresses the `ftp_server/reply`. The only error
  token is `FTP_ERR_FILE_ACCESS`.

```
mqtt-ftp-service [-v|--verbose] <destination-folder> [<chunk-size=512>]
```

Bring-up is the shared `ServiceEnv`: vm + `common::MQTTClient` to
**`localhost:1883`** (keepalive 60) + `IClock`.

> The actual multiframe (chunk reassembly + size limit + open-transfer eviction)
> lives in the CAN transport-protocol layer (`lib/src/tp/tp.c`), reached via the
> framework — vendor, not part of this service.

## Scope

VanMoof-authored logic — the CLI ([`src/main.c`](src/main.c)) and the
`MqttFtpService` ([`src/mqtt_ftp_service.c`](src/mqtt_ftp_service.c)) command/
write/read/reply flow — is reconstructed. `common::MQTTClient`/`CRC32`/`IClock`,
`std::filesystem`/`fstream`, nlohmann-json and the CAN TP layer are vendor —
modelled as opaque externs. Reconstruction OEM-validated (see
[`docs/progress.md`](docs/progress.md)).

- **Ghidra export:** [`ghidra/exports/mqtt-ftp-service_program.json`](ghidra/exports/mqtt-ftp-service_program.json)
- **MQTT topics:** [`../docs/mqtt-topics.md`](../docs/mqtt-topics.md) (the `update` section + `ftp_server/*`)

## Build

```sh
make            # src/{main,mqtt_ftp_service}.c -> build/*.o, clean -Wall -Wextra -Wpedantic
```
