# Services & binaries inventory

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
| `power` | `vmxs5-embedded-power` | **Battery + charger + power-state machine.** Manages **two** batteries — the internal **LiPo** (TI bq27542 gauge + BQ25672 charger on I²C bus 2) and the **primary** Panasonic pack (over CAN) — and sequences operational/charging/standby/low-power/deep-sleep/shipping. Also `rtc_handler` (wake alarms), `wake_on_motion_handler` (IMU), `eshifter_calibration`. Publishes `power/state` + `power/battery/{lipo,primary}/info/*`. See [`hardware.md`](hardware.md). |
| `ride` | `vmxs5-embedded-ride` | **Pedal-assist controller.** `RideService(IMotor, IPower, IPedalSensor, RideStrategy)`. Drives the motor over the **SSP** serial protocol (`ssp_protocol` on `serial_port`); reads motor + pedal sensors over **CANopen** (`MotorSensorOd`/`PedalSensorOd`). `-t 0` = speed control (`SpeedRideStrategy`), `-t 1` = **speed-ratio** control (`SpeedRatioRideStrategy`, the shipped default); `-s <kmh>` simulates/spins. 5 assist levels (0–4). Gates on Pedalling/PoweredOn/Spinning/SufficientSOC/Motor-comm. |
| `ux` | `vmxs5-embedded-ux` | **Lock & user experience.** Lock/unlock, touch-unlock, `BackupUnlock` (manual kick/backup-code unlock state machine), `Ring`, sounds/animations. Owns `ux/lock/*`, `ux/info/*`, `ux/sensor/*`. `--path` overrides storage. |
| `tracking` | `vmxs5-embedded-tracking` | **Anti-theft / tracking.** `StateClient`-based alarm state machine (`ALARM`, state timeouts); reports theft/location upstream via the bus → gateway. |
| `monitor` | `vmxs5-embedded-monitor` | **System health.** Watches components (`Modem`, …) and surfaces per-component error-state changes ("Component '%s', error state changed"); `--service`. |
| `update` | `vmxs5-embedded-update` | **Peripheral + system OTA.** Walks `manifest.txt`, version-checks each device over MQTT, flashes the sub-ECUs over CAN (page+CRC, target-side A/B boot-control, 2-stage with rollback) and the Nordic parts over **SMP/MCUmgr**; shells out to `runFOTA.sh` for the i.MX8 self-update. `Type=notify`. Full detail in [`update.md`](update.md). |
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
