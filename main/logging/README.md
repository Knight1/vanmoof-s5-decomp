# logging — log collector / dictionary expander (i.MX8)

`/usr/bin/logging` — the VanMoof S5 on-board **log collector**, package
`vmxs5-embedded-logging` (systemd `logging.service`, `After=vcan-starter
mosquitto`). Stripped AArch64 C++ ELF (`AARCH64:LE:64:v8A`, base `0x100000`,
~437 functions), Ghidra `/S5-v1.5/OS/logging`.

## What it does

The tiny sub-ECUs can't afford to send full log text over CAN, so they emit
**compact log references** — `(device-id, source-file-path-hash, line, args)`.
`logging` holds the **dictionary** that maps those refs back to text:

- The dictionary is a set of **`device|file|line|LEVEL|message`** lines, loaded
  from an optional **`log_config.txt`** argument, or a **compiled-in default**
  (`DAT_0012fe70`) when none is given.
- Parsing: split on `\n`, then `|` into 5 fields; the key is
  **`(device, hash(file), line)`** where `hash` is a back-to-front polynomial
  over the path (`×0x1003f`); the path is normalised by stripping a leading
  `CMAKE_SOURCE_DIR/` or `devices/`. Malformed lines → *"log config line
  ignored"*; collisions → *"Duplicate log config for file %s (%d), line %d"*.
- On each incoming reference the handler looks up `(device, hash, line)`,
  formats the message with the args (or `???` on a miss), and emits a **JSON**
  object — `{ "ts":"%H:%M:%S", "file_id":<device>, "file":…, "line":…,
  "level":…, "msg":… }` — to **`std::cout`**, which systemd **journald**
  captures into `/var/log` on the eMMC-backed config partition.

```
logging [-s|--service] [-f|--filter <address>] [log_config.txt]
  -s/--service   run as a service
  -f/--filter    only emit logs from the given address / device name (lower-cased)
```

The MQTT/CAN bring-up is the shared `ServiceEnv`: vm_init + `common::MQTTClient`
to **`localhost:1883`** (keepalive 60) + SocketCAN + signal handlers
`{SIGINT, SIGILL, SIGABRT, SIGTERM}`. `logging` has **no hardcoded MQTT topic
strings** — its log feed arrives over the vm transport; on the MQTT bus it is the
consumer of the `logging/event` / `error/#` topics other services publish (see
[`../docs/mqtt-topics.md`](../docs/mqtt-topics.md)).

## Scope

VanMoof-authored logic — the CLI ([`src/main.c`](src/main.c)) and the dictionary
loader/parser + expansion handler ([`src/logging_server.c`](src/logging_server.c))
— is reconstructed. The common `ServiceEnv`/`MQTTClient`/vm transport,
`std::map`/`std::string`/`std::ifstream`, and **nlohmann-json** are vendor —
modelled (the dictionary as a growable array, JSON via stdio). OEM addresses are
quoted per function.

- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/logging_program.json`](ghidra/exports/logging_program.json)

## Build

```sh
make            # src/{main,logging_server}.c -> build/*.o, clean -Wall -Wextra -Wpedantic
```
