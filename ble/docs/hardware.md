# ble — hardware & image layout

> Target `ble.20240129.145222.1.5.0.main.v1.5.0-main.bin` (VanMoof S5/A5 BLE
> SoC). Analysed in Ghidra (`vanmoof` project, `S5-v1.5` folder). All addresses
> here are **device/link addresses** (see *Image base* below) unless a `file
> 0x…` offset is given. Status CONFIRMED unless marked TBC.

## MCU identity

Nordic **nRF52-class** Cortex-M4F SoC running **Zephyr RTOS** with the Nordic
**SoftDevice Controller** (link-layer) + **MPSL**. Identified from devicetree
node-name strings and the BT controller banner in the image:

| Evidence | String |
|---|---|
| BT link-layer | `SoftDevice Controller`, `MPSL Work`, `BT CTLR ECDH` |
| Zephyr | `*** Booting Zephyr OS build %s %s ***`, `WEST_TOPDIR/zephyr/subsys/bluetooth/host/{hci_core,conn}.c` |
| CryptoCell | `crypto@5002a000` (Arm **CC310** TRNG/PKA — present on nRF52840 / nRF52833) |
| Clock/RNG | `clock@40000000`, `random@4000d000` (`entropy_bt_hci`), `nrf_clock.h` |
| GPIO | `gpio@50000000` (P0) |
| NVM | `flash-controller@4001e000` (NVMC), `FLASH_0` |
| WDT | `watchdog@40010000` |

**MCU: nRF52840 (TBC; could be nRF52833).** Both carry the CC310 at
`0x5002a000`; the cellular modem is a *separate* `nRF9160` target (`modem`), so
this SoC is the BLE-only radio. Distinguishing 52840 vs 52833 (USB / flash-RAM
size) is not yet confirmed from the image.

Peripheral base addresses (standard nRF52 APB map, from the devicetree strings):

| Base | Block |
|---|---|
| `0x40000000` | CLOCK / POWER |
| `0x4000d000` | RNG (`random`) |
| `0x40010000` | WDT |
| `0x4001e000` | NVMC (`flash-controller`) |
| `0x50000000` | GPIO P0 |
| `0x5002a000` | CC310 CryptoCell |
| `0xe000e100` | NVIC ISER (seen in fault/init code) |

RAM: initial MSP `0x200130a0` (nRF52 SRAM at `0x20000000`).

## Image base — **0x23000** (critical)

The file was imported into Ghidra at base `0x0`, but the firmware is **linked
at `0x23000`** (its MCUboot application-slot base). This had to be corrected
before anything resolved — see [[ble-image-base]] in project memory.

```
file 0x00000  ┌─────────────────────────┐ device 0x23000  ← MCUboot image header
              │ MCUboot header (0x200)   │                   magic 0x96f3b83d
file 0x00200  ├─────────────────────────┤ device 0x23200  ← Zephyr _vector_table
              │ .text  (Zephyr + SDC +   │
              │         VanMoof app)     │
file ~0x3f000 ├─────────────────────────┤
              │ .rodata / strings        │  strings at file 0x40000+ (dev 0x63000+)
file 0x44200  ├─────────────────────────┤   ih_img_size = 0x44200
              │ MCUboot TLV trailer      │
file 0x44496  └─────────────────────────┘   (EOF)
```

MCUboot `image_header` (file `0x0`): `ih_magic=0x96f3b83d`, `ih_load_addr=0`
(XIP), `ih_hdr_size=0x200`, `ih_img_size=0x44200`, version `1.5.0`.

**Consequences of the base:**
- Addressing is **literal-pool `LDR [pc]`** throughout — there are **zero
  `movt` instructions**, so an absolute address `X` in the device == a literal
  word `X` in flash. At the wrong base every literal pointed outside the image,
  so Ghidra resolved *no* string/data xrefs.
- After `set_image_base 0x23000` + reanalyze, the function count rose
  **976 → 2015** (resolved references exposed previously-missed code) and
  string/data xrefs resolve (e.g. the boot banner at dev `0x64eb8` is referenced
  by the Zephyr boot path).

Vector table (file `0x200`, dev `0x23200`): MSP `0x200130a0`, Reset
`0x418f4`, fault-default `0x418c8` (shared by HardFault/MemManage/BusFault/
UsageFault), PendSV `0x41564` (`z_arm_pendsv`), the common IRQ entry `0x41670`,
and SoftDevice/MPSL ISRs at `0x5f754/0x5f78c/0x5f7a6/0x5f966` + NMI `0x5b28e`.

## Notes / TBC
- nRF52840 vs nRF52833 not yet confirmed.
- Exact MCUboot partition map (why the slot is at `0x23000` — likely b0/NSIB +
  MCUboot ahead of it) is off-image; only the app slot is in this binary.
