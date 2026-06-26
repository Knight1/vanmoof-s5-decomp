# Services & binaries inventory

> **MQTT topic catalog:** every topic each service publishes/subscribes, with
> payloads and retain flags, is in [`mqtt-topics.md`](mqtt-topics.md). CAN/OD
> signals are in [`can-bus.md`](can-bus.md).


All VanMoof services live in `/usr/bin` as **AArch64 ELF** executables and are
started by systemd units in `/lib/systemd/system`. One binary (`gateway`) is
**Go 1.19**; the rest are **C++** (linked against `libstdc++`, `libmosquitto`,
`libsystemd`). They share a common C++ runtime layer (`namespace common`:
`IMQTTClient`, `ITimerFactory`, `StateClient`, `Timer`, `Clock`, `VmTimer`),
i.e. every service is built on the same MQTT-client + state-machine framework.

## systemd units (enabled in `multi-user.target.wants`)

| Unit | `ExecStart` | Notes |
| --- | --- | --- |
| `gateway.service` | `/usr/bin/gateway` | `Requires=`+`After=mosquitto`, `After=ppp@nrf9160` |
| `power.service` | `/usr/bin/power` | `Type=notify`; `After=` mosquitto + all three bridges |
| `ride.service` | `/usr/bin/ride -t 1` | `After=power` |
| `ux.service` | `/usr/bin/ux` | `After=power update` |
| `tracking.service` | `/usr/bin/tracking` | `After=mosquitto power` |
| `monitor.service` | `/usr/bin/monitor --service` | `After=power` |
| `update.service` | `/usr/bin/update /opt/devices_fw` | `Type=notify`; `After=power` |
| `logging.service` | `/usr/bin/logging --service` | `After=vcan-starter mosquitto` |
| `mqtt-ftp.service` | `/usr/bin/mqtt-ftp-service /tmp` | `After=mosquitto` |
| `spi-can-bridge@bridge.service` | `/usr/bin/spi-can-if-linux bridge` | `After=vcan-starter` |
| `spi-mqtt-bridge@ble.service` | `/usr/bin/spi-mqtt-bridge ble` | `After=mosquitto`; `ExecStartPost=sleep 5` |
| `spi-mqtt-bridge@modem.service` | `/usr/bin/spi-mqtt-bridge modem` | `After=mosquitto` |
| `vcan-starter.service` | `/usr/sbin/start_vcan.sh` | oneshot; `modprobe vcan; ip link add vcan0` |

Stock units also enabled: `mosquitto`, `avahi-daemon`, `iptables`,
`busybox-syslog`, `busybox-klogd`, `ppp@nrf9160`, `systemd-networkd`,
`systemd-resolved`.

## `gateway` — cloud bridge (Go)

The only Go binary; `github.com/VanMoof/embedded/gateway`, go1.19, CGO on. It
bridges the local MQTT bus to **AWS IoT** and proxies BLE. Internal packages
(from the embedded build paths):

| Package | Role |
| --- | --- |
| `internal/iot` + `iot/client` | AWS IoT MQTT client (TLS) |
| `internal/iot/ca` (`ca/pool`) | device CA / cert pool for IoT mTLS |
| `internal/iot/shadow` (`client`,`document`,`state`,`report`) | **Device Shadow** sync (`$aws/things/<thing>/shadow`) |
| `internal/iot/job` (`client`,`document`,`execution`) | **AWS IoT Jobs** — backend-pushed commands (`$aws/things/<thing>/jobs/…`) |
| `internal/telemetry` (`batch`,`collector`,`router`,`mode`,`ignore`,`config`) | telemetry batching → `$aws/rules/telemetry/<…>` |
| `internal/gateway/configshadow` | config delivered via a shadow document |
| `internal/gateway/logcollector` + `logger` | log forwarding to cloud |
| `internal/bike` (`firmware`,`provisioning`) | reads bike firmware version + provisioning data |
| `internal/ble` + `ble/proxy` | BLE proxying (the `ble/proxy` topics) |
| `internal/mqtt` (`client`,`pattern`) | local-bus MQTT client |
| `internal/event` (`ble`,`proxy`,`timezone`) | event routing / timezone |

Dependencies (Go modules embedded): `eclipse/paho.golang v0.10.0` (MQTT v5),
`fxamacker/cbor/v2`, `tidwall/gjson`+`sjson` (JSON), `google/uuid`,
`go.uber.org/zap` (logging), `peterbourgon/ff` (flags), `golang.org/x/sync`,
`golang.org/x/sys`.

## C++ service suite

| Binary | Pkg | What it does (from symbols/strings) |
| --- | --- | --- |
| `power` | `vmxs5-embedded-power` | **Battery + charger + power-state machine.** Manages **two** batteries — the internal **LiPo** (TI bq27542 gauge + BQ25672 charger on I²C bus 2) and the **primary** Panasonic pack (over CAN) — and sequences operational/charging/standby/low-power/deep-sleep/shipping. Also `rtc_handler` (wake alarms), `wake_on_motion_handler` (IMU), `eshifter_calibration`. Publishes `power/state` + `power/battery/{lipo,primary}/info/*`. **Ghidra reconstruction: [`../power/`](../power/).** |
| `ride` | `vmxs5-embedded-ride` | **Pedal-assist controller.** `RideService(IMotor, IPower, IPedalSensor, RideStrategy)`. Drives the motor over the **SSP** serial protocol (SLIP + CRC-16/MODBUS over `/dev/ttymxc3`); reads motor + pedal sensors over **CANopen** OD. `-t 0` speed control (`SpeedRideStrategy`), `-t 1` **speed-ratio** (`SpeedRatioRideStrategy`); `-s <kmh>` spins. `EnableMotor = PoweredOn && Pedalling && SufficientSOC` (SOC≥13); assist curve is data-driven (runtime `.bss` tables). **Full decode: [`ride.md`](ride.md) + [`../ride/`](../ride/).** |
| `ux` | `vmxs5-embedded-ux` | **UX orchestrator / behavioural brain.** `UXService` 6-state strategy machine (standby/operational/charging/alarm/maintenance/updating) coordinating lock (`BackupUnlock` = plaintext 3-symbol code, no crypto), lights (lux-hysteresis auto-headlight), sound, sensors, ride (incl. the persistent trip **odometer**), alarm/theft escalation, Apple Find-My glue, power button. Owns `ux/lock/*`, `ux/info/*`, `ux/sensor/*`, `ux/alarm/*`, `ux/sound/*`. `--path` overrides storage. **Full decode: [`ux.md`](ux.md) + [`../ux/`](../ux/).** |
| `tracking` | `vmxs5-embedded-tracking` | **Anti-theft / tracking.** `StateClient`-based 3-state machine (OFF/AUTO/THEFT) fed by IMU movement (`ux/alarm/state`==3 → AUTO), inbound SMS (`modem/sms`: enabled→THEFT, disabled→OFF, locate→sound) and cellular polling (`CellLocator`, 2/30/240/5 min). Publishes retained `tracking/state`. No outbound SMS / no location estimate here (modem's job). **Full decode: [`tracking.md`](tracking.md) + [`../tracking/`](../tracking/).** |
| `monitor` | `vmxs5-embedded-monitor` | **Component health/version supervisor.** `MonitorService` polls each component (BLE / LPC Cortex-M sub-ECUs / Nordic nRF9160 modem) for heartbeat aliveness (2 s) + firmware/bootloader/vendor versions, drives **GPIO reset lines** (sysfs, pins 1/0/10/11), and renders a component status table over MQTT. Modem aliveness = ping. **Full decode: [`monitor.md`](monitor.md) + [`../monitor/`](../monitor/).** |
| `update` | `vmxs5-embedded-update` | **Peripheral + system OTA.** Walks `manifest.txt`, version-checks each device over MQTT, flashes the sub-ECUs over CAN (page+CRC, target-side A/B boot-control, 2-stage with rollback) and the Nordic parts over **SMP/MCUmgr**; shells out to `runFOTA.sh` for the i.MX8 self-update. `Type=notify`. Full detail in [`update.md`](update.md). |
| `lightweight_update` | `vmxs5-embedded-lightweight-update` | **Standalone CLI flasher** (not a systemd service). Trimmed update path: `lightweight_update [-fv] [-b bus] [-t target] <file>` flashes one firmware file to one device over CAN/tty, reusing the `update` clients (`UpdateClientFactory` + `IUpdateClient` subclasses). MQTT id `lightweight_update`/`update-service`, broker `localhost:1883`. Reconstruction: [`../lightweight_update/`](../lightweight_update/). |
| `motor_update_example` | `vmxs5-embedded-motor-update-lib` | **Standalone TI C2000 SCI flasher** (not a systemd service). `motor_update_example [-b] [-p tty] <kernel> <app>` flashes the TMS320F280049C motor controller over its serial bootloader: boot-mode GPIO → autobaud → upload the SCI flash kernel → DFU-program the app. Does **not** use the MQTT bus. Reconstruction: [`../motor_update_lib/`](../motor_update_lib/). |
| `logging` | `vmxs5-embedded-logging` | **Log collector.** Pulls log/error topics off the bus into `/var/log` (eMMC-backed). |
| `mqtt-ftp-service` | `vmxs5-embedded-mqtt-ftp` | **File transfer over MQTT** rooted at `/tmp` — the `ftp_server/command` / `ftp_server/reply` channel (log/cert/config push-pull). |

## Bridges (non-IP links → the bus)

| Binary | Pkg | Bridges |
| --- | --- | --- |
| `spi-mqtt-bridge <ifc>` | `vmxs5-embedded-spi-mqtt-bridge` | SPI ↔ MQTT for `ble` (nRF52) and `modem` (nRF9160). Each device's protocol frames become MQTT messages and back. |
| `spi-can-if-linux <ifc>` | `vmxs5-embedded-spi-can-bridge` | SPI ↔ CAN ↔ SPI to the `imx8_bridge` co-processor — exposes the CAN fleet on `vcan0` and bridges CAN traffic to the bus. |
| `start_vcan.sh` | `vmxs5-vcan-starter` | `modprobe vcan; ip link add vcan0 type vcan; ip link set up vcan0`. |

## `/opt/devices_fw` — the peripheral firmware bundle (`vmxs5-device-binaries`)

The sub-ECU images `update` distributes, plus `manifest.txt`
(`Filename,Device,Date,Time,Major,Minor,Patch,Type,AllowSkip,DontRollback`).
Contents match the repo-root release table: `imx8_bridge`, `ble`, `modem`,
`motor_sensor`, `motor_control`, `elock`, `eshifter`, `user_ecu`, `frontlight`,
`rearlight`, `power_control`, `power_pedal` (all `1.5.0.main`, 2024-01-29), plus
vendor blobs `battery_primary_panasonic` (1.4.256), `charger_liteon_normal`
(1.8.0), `charger_liteon_speed` (1.2.0), and `mfw_nrf9160_1.3.1.zip` (Nordic
modem firmware). Battery/charger/modem carry `AllowSkip=1`; battery+chargers
also `DontRollback=1`.

## Not VanMoof (stock, do not reconstruct)

`dumpkeys` (the standard Linux `kbd-2.0.4` keymap tool — *not* a key-dumping
utility despite the name), `mosquitto`, `avahi`, `iptables`, BusyBox, systemd,
glibc, OpenSSH, `pppd`/`chat`, and the kernel + its modules are all stock
upstream. Per project policy these are **vendor / deferred**.
