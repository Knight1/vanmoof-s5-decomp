# spi_can_bridge — SPI ↔ CAN bridge (i.MX8)

`/usr/bin/spi-can-if-linux` — the VanMoof S5 **SPI ↔ CAN bridge**, package
`vmxs5-embedded-spi-can-bridge` (systemd `spi-can-bridge@bridge.service`,
`spi-can-if-linux bridge`, `After=vcan-starter`). Stripped AArch64 C++ ELF
(`AARCH64:LE:64:v8A`, base `0x100000`, ~386 functions), Ghidra
`/S5-v1.5/OS/spi-can-if-linux`.

## What it does

It puts the bike's **CAN fleet onto a SocketCAN interface (`vcan0`)** — but the
physical CAN bus is behind the **`imx8_bridge`** co-processor (a discrete
Cortex-M), reached over **SPI** (`/dev/spidev1.0`). So this service is the host
side of that link:

- Selects the named config (default **`bridge`**; the config table is keyed by
  name, `config_lookup_by_name` @0x107120). Initialises the **GPIO data-ready**
  line, logs `"Starting on %s, irq_pin=%d"` (main.cpp:0x62), traps
  `{SIGINT, SIGTERM, SIGQUIT}`.
- Brings up, in order: **vmlib**, a **composite channel**, the **SocketCAN**
  endpoint (`socket(PF_CAN, SOCK_RAW, CAN_RAW)` → `SIOCGIFINDEX("vcan0")` →
  `bind`), and the **SPI-master** endpoint to the imx8_bridge; then **starts the
  composite channel** that couples them. Each step has its own error log
  (`"Failed to initialize SocketCAN"` 0x73, `"… SPIM channel"` 0x7a,
  `"Failed to start composite channel"` 0x7f, …).
- The **CAN transport-protocol (TP)** layer reassembles the 8-byte CAN frames
  into larger multiframe transfers keyed by `(src, idx, trgt)` — this is the
  **same `lib/src/tp` layer** the `mqtt-ftp` service defers to; its home is here
  (`spim_channel.cpp`). `"Multiframe transfer too large …"` /
  `"Too many open multiframe write transfers …"`.

> See [`../docs/can-bus.md`](../docs/can-bus.md) for the CAN map this exposes.
> The `imx8_bridge` MCU itself is a separate FreeRTOS Cortex-M image.

## Scope

The CLI/config + init ladder ([`src/main.c`](src/main.c)) and the **SocketCAN
endpoint** ([`src/spi_can_bridge.c`](src/spi_can_bridge.c) — `socket`/`ioctl`/
`bind`/`can_frame`, reconstructed faithfully with the kernel CAN ABI inlined) are
reconstructed. The SPI-master frame protocol, the **CAN-TP multiframe
reassembly**, the GPIO data-ready interrupt and the `std::thread` plumbing are
vendor (`lib/src/tp` + `spim_channel.cpp` + `gpio_polling.cpp`) — modelled as
opaque externs.

- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/spi-can-if-linux_program.json`](ghidra/exports/spi-can-if-linux_program.json)

## Build

```sh
make            # src/{main,spi_can_bridge}.c -> build/*.o, clean -Wall -Wextra -Wpedantic
```
