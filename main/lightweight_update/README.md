# lightweight_update — standalone CLI firmware flasher (i.MX8)

`/usr/bin/lightweight_update` — the VanMoof S5 **trimmed update path**, package
`vmxs5-embedded-lightweight-update`. Stripped AArch64 C++ ELF
(`AARCH64:LE:64:v8A`, base `0x100000`, ~471 functions), Ghidra
`/S5-v1.5/OS/lightweight_update`.

A one-shot command-line flasher: push **one** firmware file to **one** device
over CAN (or the serial tty), *without* the `update` service's manifest walk,
FOTA orchestration, or systemd-notify. It reuses the exact same update machinery
as `update` — `UpdateClientFactory`, the `IUpdateClient` subclasses
(`LightweightUpdateClient`, `C2000UpdateClient`, `MqttUpdateClient`,
`ThirdPartyUpdateClient`/Dynapack/Panasonic/Liteon), `VersionClientMqtt`, and the
vm/CAN context — all from `devices/main/update/src/*`.

## CLI

```
Usage: lightweight_update [-fv] [-b <can_channel>] [-t <target>] <file_to_send>
  -b <can_bus>   CAN bus to use (vcan0 by default)
  -f             force update even if the firmware version already matches
  -p <ttydev>    serial tty used for updating (defaults to /dev/ttymxc3)
  -t <target>    device name OR numeric address (0x.. hex / decimal); if omitted
                 the device name is derived from the filename
  -v             verbose logging
  <file_to_send> filename must include the device name (unless -t) and version,
                 e.g. 'device_name.date.time.major.minor.patch.bin' or 'device_name.bin'
```

Exit code `0` on `Update successful.`, `1` otherwise
(`Update was not performed.` / `could not get an update client for: …` /
`Could not init channel`). Reports `Spent time:<ms> ms`.

## Scope

The only VanMoof-authored source unique to this binary is
**`utils/lightweight_update/main.cpp`** — reconstructed in
[`src/main.c`](src/main.c). The update clients + vm/MQTT/version plumbing are the
**shared `update` code** ([`../update/`](../update/)); they are modelled here as
opaque externs in [`include/lightweight_update.h`](include/lightweight_update.h),
not re-reconstructed.

- **Architecture / topics:** [`../docs/mqtt-topics.md`](../docs/mqtt-topics.md)
  (`update` section — same `device/+/version/*` namespace).
- **Per-function tracker:** [`docs/progress.md`](docs/progress.md)
- **Ghidra export:** [`ghidra/exports/lightweight_update_program.json`](ghidra/exports/lightweight_update_program.json)

## Build

```sh
make            # src/main.c -> build/main.o, clean under -Wall -Wextra -Wpedantic
```
