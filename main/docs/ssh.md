# SSH access (`sshd`)

`v1.5.0-main` runs **OpenSSH 8.0p1** (stock Yocto zeus `openssh-sshd
8.0p1-r0`). It is **enabled** — socket-activated on TCP 22, every interface —
and **certificate-only**: password and keyboard-interactive auth are off, and
the one credential it accepts is a user certificate signed by a single VanMoof
CA. It is **not** affected by the Nov 2026 musl password bypass ("sshole") —
see [the assessment](#assessment-sshole-nov-2026) below.

## Startup

| Unit / file | Content | Effect |
| --- | --- | --- |
| `/etc/systemd/system/sockets.target.wants/sshd.socket` | enable link (stock preset `98-openssh-sshd.preset`: `enable sshd.socket`) | on at every boot — through `sockets.target`, which is why it is absent from `multi-user.target.wants` |
| `sshd.socket` | `ListenStream=22`, `Accept=yes` | systemd holds TCP 22 on all addresses and spawns one `sshd@.service` per connection |
| `sshd@.service` | `ExecStart=-/usr/sbin/sshd -i $SSHD_OPTS`, `EnvironmentFile=-/etc/default/ssh` | `/etc/default/ssh` does not exist → no extra options → config = `/etc/ssh/sshd_config` |
| `sshdgenkeys.service` | `sshd_check_keys` (pulled in by `sshd@.service`) | on the first connection, creates any missing `HostKey` named in `sshd_config` → the ed25519 host key lives on `mmcblk2p6` and survives reboots and updates |

`/etc/ssh/sshd_config_readonly` is Yocto's read-only-rootfs variant (host keys
under `/var/run/ssh/`); nothing passes `-f`, so it is unused.

## Configuration

`/etc/ssh/sshd_config` — VanMoof content in the stock `openssh-sshd` package
(the upstream file is the long commented template):

```
HostKey /run/media/mmcblk2p6/ssh_host_ed25519_key
TrustedUserCAKeys /etc/ssh/trusted
AuthorizedPrincipalsCommand /etc/ssh/authorized-principals-cmd %u
AuthorizedPrincipalsCommandUser root
PermitRootLogin yes
PasswordAuthentication no
ChallengeResponseAuthentication no
Compression yes
ClientAliveInterval 15
ClientAliveCountMax 4
Banner /run/media/mmcblk2p6/serial
```

- **Only `publickey`** is enabled (hostbased/GSSAPI stay at their default
  *off*). sshd is built **without PAM** — `NEEDED` is libcrypto 1.1, libutil,
  libz, `libcrypt.so.2`, libc; `/etc/pam.d` holds only `systemd-user`. No
  `authorized_keys` ships (`/home/root` is empty), so the working credential is
  a **CA-signed user certificate**.
- **Banner:** the bike's serial (`mmcblk2p6/serial`) is sent to every client
  *before* authentication.

### Certificate trust

`/etc/ssh/trusted` (package `openssh`, VanMoof addition) — one user CA:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAICzgtrnn21dIFe3Pho7FasLTVsixoTDaZMqwPOq4qb6H
256 SHA256:Jn7M0JqkmtUNYQ0HvkZ8Jg5/WQk7c+3GTT/CfXnHxg0 (ED25519)
```

`authorized-principals-cmd <user>` (package `openssh`, runs as root) prints the
principals a certificate may carry to log in as `<user>`:

| Principal | Printed when | Scope |
| --- | --- | --- |
| `<user>` (e.g. `root`) | always | **every bike** |
| `<serial>-<user>` | `mmcblk2p6/serial` exists | one bike |
| `<bike_id>-<user>` | `mmcblk2p6/bike_id` exists | one bike |
| `factory-<user>` | `mmcblk2p6/bike_id` absent | every unprovisioned bike |

One certificate with principal `root` from this CA is therefore a root login
on every S5 running this image; the trust boundary is the CA private key.
(Validity windows and critical options such as `source-address` are fixed at
signing time and are not visible from the bike.)

## Reachability

- **Every interface, no firewall:** `/etc/iptables/iptables.rules` is empty
  (0 B) and there is no `ip6tables.rules`.
- **Wired:** `02-wired.network` (package `systemd-conf`; stock ships
  `80-wired.network`) matches `en*`/`eth*` — the on-board FEC and any
  USB-Ethernet adapter — runs a DHCP client **and** always adds the static
  `192.168.137.230/24`. That is the Windows Internet Connection Sharing subnet:
  a bike cabled to a PC answers at `192.168.137.230` with or without DHCP.
- **Cellular:** the nRF9160 PPP link (`ppp@nrf9160`, `defaultroute` +
  `replacedefaultroute`) is the default route and port 22 is open on it too;
  whether it is reachable from outside depends on the carrier network (IoT
  APNs are usually NATed).

## `/etc` at runtime

The kernel `Image` carries a built-in initramfs (uncompressed cpio-newc at
`Image`+`0xb535bc`, 574 entries). Its `/init` — a customized
meta-readonly-rootfs-overlay init, consuming the `root=` / `rootrw=` /
`rootrwfstype=` / `rootrwreset=` arguments `bootpp` builds (see
[`secure-boot.md`](secure-boot.md)) — mounts the squashfs read-only with an
overlay on top. The upper layer depends on `/etc/firmware_imagetype` *inside
the squashfs*:

| Image type | Upper layer | Effect |
| --- | --- | --- |
| `production` (v1.5.0) | **tmpfs** | every boot starts from the HAB-signed squashfs; only `config/machine-id` is copied in from `mmcblk2p6` |
| `develop` | ext4 `rootrw=` (`mmcblk2p6/upperdir`) | changes to `/` persist |

So on a production bike nothing stored on the device can alter `sshd_config`,
`trusted` or `/etc/shadow`, or plant an `authorized_keys`, across a reboot.
What sshd *does* read from the unsigned `mmcblk2p6`: the host key, the banner,
and `serial` / `bike_id` for the principals above.

## Accounts

`/etc/shadow`: `root` has a `$6$` (sha512crypt) hash; 17 system accounts
(`daemon`, `bin`, `sys`, … `nobody`) are `*`, most with `/bin/sh`; 8 service
accounts (`sshd`, `mosquitto`, `avahi`, …) are `!`. With password auth off,
none of these passwords is usable over SSH.

## Assessment: sshole (Nov 2026)

[sshole](https://sshhole.com/) ([PoC + patch](https://github.com/oz9un/sshole)):
OpenSSH built against **musl** without PAM accepts any 257–1024-byte password
for an account whose shadow field is exactly `*`. sshd hashes the attempt with
a salt borrowed from another account's `$5$`/`$6$` hash (`pick_salt()`); musl's
SHA-crypt refuses keys over 256 bytes, its `crypt()` then returns `"*"`, and
that compares equal to the stored `*`. Fixed in OpenSSH 10.6p2.

| Needs (all five) | `v1.5.0-main` | |
| --- | --- | --- |
| musl libc | glibc 2.30 + **libxcrypt 4.4.8** (`crypt@XCRYPT_2.0`) | **not met** |
| sshd without PAM | no PAM | met |
| `PasswordAuthentication yes` | **`no`** | **not met** |
| an account with `*` | 17 | met |
| an account with `$5$`/`$6$` | `root` | met |

**Not exploitable** — two independent blockers, either sufficient:

1. **Password auth is off.** sshd rejects the `password` method before
   `sys_auth_passwd()` / `crypt()` runs. 8.0p1 does contain the affected path
   (the upstream patch applies from 7.3p1); the config keeps it unreachable.
2. **libxcrypt never returns `"*"`.** In the shipped
   `/usr/lib/libcrypt.so.2.0.0`: `crypt` (`0x14330`) tail-calls `crypt_r`
   (`0x15110`), which first runs `make_failure_token` (`0x10df0`:
   `mov w2,#0x302a` + `strh` → `"*0"`; `"*1"` if the setting starts with `*0`),
   then `do_crypt` (`0x14e60`: `strlen`, `cmp x0,#0x1ff` → `ERANGE` for
   ≥ 512 bytes), and returns that buffer — so every failure is `*0`/`*1`.
   Cross-check on libxcrypt 4.4.10: 257–511-byte inputs hash normally
   (`$6$…`), ≥ 512 return `*0`.

Provenance: `etc/ssh/*`, `etc/shadow`, the sshd units, `usr/sbin/sshd` and
`usr/lib/libcrypt.so.2.0.0` from `root.sqfs`, and the initramfs from the
`boot.sqfs` kernel, of `v1.5.0-main` (sha256 `45ab7c4d…`; payload md5s
`ef2bdfbf…` / `dfe3c2cc…` = FOTA header), unpacked per
[`fota-image.md`](fota-image.md).

**On-bike check** (needs only the network path):

```sh
ssh -v -o PubkeyAuthentication=no root@192.168.137.230
#   … remote software version OpenSSH_8.0
#   … Authentications that can continue: publickey
#   root@192.168.137.230: Permission denied (publickey).
```

`publickey,password` in that list, or a version other than `OpenSSH_8.0`,
means the bike runs a different build from the one analysed here.

Not affected either: **regreSSHion** (CVE-2024-6387), which is 8.5p1–9.7p1
only.
