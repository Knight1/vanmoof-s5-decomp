# Loadable kernel modules

The rootfs ships only **two** out-of-tree `.ko` files (everything else is built
into the 5.4.70 kernel). **Both are upstream vendor code** — per project policy
they are identified and **deferred**, not reconstructed.

Location: `/lib/modules/5.4.70+/extra/`. Both are AArch64 ELF relocatables,
`vermagic = 5.4.70+ SMP preempt mod_unload modversions aarch64`.

## `cryptodev.ko` — `/dev/crypto` (vendor)

| | |
| --- | --- |
| Project | **cryptodev-linux** (upstream) |
| Author | Nikos Mavrogiannopoulos `<nmav@gnutls.org>` |
| License | GPL · `description = "CryptoDev driver"` |
| Param | `cryptodev_verbosity` (0 normal / 1 verbose / 2 debug) |

Exposes a `/dev/crypto` ioctl interface so **userspace** (e.g. an OpenSSL engine)
can offload symmetric ciphers and hashes to the i.MX8 **CAAM** crypto hardware.
Standard cryptodev API symbols are present (`cryptodev_cipher_decrypt`,
`cryptodev_hash_{init,update,final}`, `crypto_get_session_by_sid`, …). On this
bike it backs hardware-accelerated TLS/crypto for the services that need it.

## `jailhouse.ko` — partitioning hypervisor driver (vendor)

| | |
| --- | --- |
| Project | **Jailhouse** (Siemens) — static partitioning hypervisor |
| Version | **v0.12** · `srcversion 2D8EF16AE59A1528BC7C129` |
| License | GPL · `description = "Management driver for Jailhouse partitioning hypervisor"` |
| Firmware | **`jailhouse.bin`** (108,552 B, present at `/lib/firmware/jailhouse.bin`) |

This is only the **Linux-side management driver**; the actual hypervisor is the
firmware blob `jailhouse.bin` it loads. Jailhouse statically partitions the SoC
so an isolated cell can run alongside Linux on a dedicated core. No `*.cell`
configuration files ship in the rootfs (cell configs are supplied at runtime or
elsewhere), and the `jailhouse` userspace tool is not in the rootfs either — so
*how* VanMoof uses the partition here isn't established from this image alone.

> Note: the **`imx8_bridge`** firmware is a **discrete Cortex-M MCU** image
> ([`../../imx8_bridge/`](../../imx8_bridge/)), **not** a Jailhouse cell/inmate
> (an inmate on the i.MX8M Nano would be AArch64; `imx8_bridge` is thumb
> Cortex-M and is flashed as a separate SPI device). What, if anything, runs in
> a Jailhouse cell on this bike is an open question.
