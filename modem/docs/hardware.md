# modem — hardware & firmware map

The bike's **cellular modem co-processor**: a Nordic **nRF9160** SiP. It is a SPI
peripheral of the i.MX8 main module and the bike's link to the cellular network
(LTE-M / NB-IoT) for anti-theft / tracking + remote services.

## MCU — Nordic **nRF9160** (SiP)

- **Application core:** ARM **Cortex-M33** (ARMv8-M Mainline, Thumb-2, TrustZone),
  1 MB flash, 256 KB RAM (`0x20000000`–`0x20040000`; image SP = `0x200024B8`).
- **Integrated LTE modem:** an LTE-M/NB-IoT baseband on a separate, isolated
  core, running Nordic's proprietary **modem firmware** (the `mfw` blob) — reached
  only via the `nrf_modem` library API; it is **vendor** and shipped/updated
  separately (`mfw_nrf9160_1.3.1.zip`).
- **GNSS:** the nRF9160 GNSS engine (shared RF front-end with the LTE modem).
- Software: **Zephyr RTOS + nRF Connect SDK**, with the **Secure Partition
  Manager** (`nrf/subsys/spm`) splitting secure/non-secure; this image is the
  non-secure application. Built with GCC 10 + newlib.

## Image — MCUboot

`opt/devices_fw/modem.20240129.145222.1.5.0.main.v1.5.0-main.bin`, 309,132 bytes:
- `0x000`: MCUboot `image_header` — magic `0x96f3b83d`, `ih_hdr_size = 0x200`,
  `ih_img_size = 0x4B4F4`.
- `0x200`: Cortex-M33 vector table (VTOR). SP `0x200024B8`, **Reset `0x2A148`**,
  NMI `0x2FAF6`, HardFault `0x2A11C`, SVCall `0x29B00`, PendSV `0x29AA8`,
  SysTick `0x2AAF8`; ~60 IRQ lines → one default handler `0x29B80`.
- `0x200`–`0x4B6F4`: code + rodata; then the MCUboot TLV trailer.
- **Base `0x0`** (file offset == virtual address) — confirmed by the reset/fault
  handlers decoding as valid Thumb-2 at their literal file offsets.

## Peripherals referenced

| Peripheral | Use |
|---|---|
| `NRF_SPIM3` / SPIS (`spi_nrfx_spis`) | the SPI **slave** link to the i.MX8 (the bike's main module is master) |
| `NRF_UARTE2` | the PPP IP-bearer UART to the i.MX8 |
| `NRF_TIMER0/1/2`, `NRF_RTC0/1`, `NRF_CLOCK`, `NRF_DPPIC` | timing / clock / event routing |
| WDT @`0x18000` | watchdog (`wdt_heartbeat_cb`) |
| `NRF_P0`, `NRF_REGULATORS`, `NRF_NVMC` | GPIO, power, flash |

## Role in the bike

The nRF9160 is purely the **bearer + command channel** — it does the cellular
link (PPP IP + the SPI command bridge), GNSS fixes, and SMS/time reception, and
hands everything to the i.MX8 Linux side over SPI/PPP. The cloud connectivity
(AWS IoT / MQTT) is entirely on the i.MX8 (`gateway`); see `../README.md` for the
application subsystem map and `../main/spi_mqtt_bridge/` for the SPI framing.
