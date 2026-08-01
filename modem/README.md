# `modem` — nRF9160 cellular co-processor (LTE-M/NB-IoT)

The bike's **cellular modem co-processor**: a Nordic **nRF9160** SiP (ARM
Cortex-M33 application core + an integrated LTE-M/NB-IoT modem), running a
**Zephyr RTOS / nRF Connect SDK** application. It is a **SPI** peripheral of the
i.MX8 main module — the Linux side reaches it through `spi-mqtt-bridge`
(`modem/*`, connection control byte `0x81`; see `../main/spi_mqtt_bridge/`) — and
it carries the bike's anti-theft / tracking link to the cloud (the i.MX8
`tracking` service drives it; see `../main/docs/tracking.md`).

> Status: **imported into Ghidra; bootstrap + classification in progress.** Raw
> image in the `vanmoof` project (`/S5-v1.5/…modem…bin`, `ARM:LE:32:Cortex`,
> base 0). Auto-analysis found 0 functions (the MCUboot header at `0x0` is not a
> vector table); it is being bootstrapped from the real vector table at `0x200`.

## Image

`opt/devices_fw/modem.20240129.145222.1.5.0.main.v1.5.0-main.bin`
- 309,132 bytes, build family `1.5.0.main` (`Jan 29 2024`). An **MCUboot image**,
  not the `VMFW` sub-ECU format:
  - `0x000`: MCUboot `image_header` — magic `0x96f3b83d`, `ih_hdr_size = 0x200`,
    `ih_img_size = 0x4B4F4`.
  - `0x200`: the Cortex-M33 vector table (VTOR) — **SP = `0x200024B8`**, **Reset =
    `0x0002A148`** (confirmed valid Thumb-2), then NMI/HardFault/… + the nRF9160
    IRQ vectors.
  - `0x200`–`0x4B6F4`: code + rodata; then the MCUboot TLV trailer.
- **Image base is `0x0`** (file offset == virtual address) — confirmed by the
  reset/default handlers decoding as valid code at their literal file offsets.
  (Unlike `ble`, which is linked at `0x23000`.)

## What it is (confirmed)

- **Nordic nRF9160**, Cortex-M33 (ARMv8-M). Zephyr + nRF Connect SDK + the
  **Secure Partition Manager** (`nrf/subsys/spm`). Peripherals referenced:
  `NRF_P0` (GPIO), `NRF_SPIM3` (the SPI link to the i.MX8), `NRF_UARTE1/2`,
  `NRF_TIMER0-2`, `NRF_RTC0/1`, `NRF_CLOCK`, `NRF_NVMC`, `NRF_TWIM2`.
- The firmware embeds `__FILE__` source paths (`WEST_TOPDIR/zephyr/…`,
  `…/nrf/…`, `…/modules/…`) — these make the **app-vs-vendor split tractable**:
  Zephyr kernel, the nRF Connect SDK (LTE link control, AT host, the modem
  library / `nrfxlib` `bsdlib`), and mbedTLS are **vendor**; the VanMoof
  application is the SPI-bridge endpoint + the cloud/cellular logic.

## Two firmwares on the nRF9160 — only one is here

The nRF9160 runs two images: this **application-core firmware** (VanMoof + Zephyr,
*reconstructable*), and the Nordic **modem firmware** (the proprietary LTE
baseband, shipped as `mfw_nrf9160_1.3.1.zip` and flashed separately) which is
**vendor and not reconstructed** (see the `modem-update` tooling note).

## Reconstruction

The VanMoof application (37 functions, located among the 1,334 Zephyr/nRF
functions via the thread/log structs) is reconstructed to behaviour-oriented
reference C in `src/` — `spi_bridge.c` (the SPI-slave COBS/CRC16 framing to the
i.MX8), `ppp_lte.c`, `cellular.c` (GNSS/SMS/time), `app_main.c` (FTP/heartbeat/DFU/
main). See `docs/progress.md` + `docs/function_map.md`. Like `motor_control`, this
is **reference C, not compiled** (the real build is a Zephyr / nRF Connect SDK app).

## Open / next

- [x] Bootstrapped (1,334 fns), architecture decoded, app vs vendor split,
      VanMoof app (37 fns) reconstructed to reference C.
- [ ] Cross-ref the recovered SPI-slave framing (COBS + CRC16 + `0x55AA55AA`
      sync) byte-for-byte against `../main/spi_mqtt_bridge/`.
- [ ] Reconstruct deeper into the LTE/GNSS/SMS orchestration if needed (the core
      threads + dispatch are done; the AT-command details are mostly vendor).
