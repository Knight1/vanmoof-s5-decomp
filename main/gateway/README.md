# `gateway` service — Ghidra reconstruction

Reverse-engineering notes for `/usr/bin/gateway` (the bike ↔ **AWS IoT** cloud
bridge, pkg `vmxs5-gateway`). **Go** binary, AArch64 ELF, ~6.9 MB, 3456 functions;
imported in Ghidra as `/S5-v1.5/OS/gateway` (image base `0x10000`). Unlike the C++
services this is **Go** — string-rich, with `gopclntab` metadata (41 k symbols),
but functions still show as `FUN_*` until the Go symbol pass is applied.

Build provenance (from embedded module paths):
`/workdir/build/tmp/work/aarch64-poky-linux/vmxs5-gateway/1.0-r0/build/…` — Yocto
`aarch64-poky-linux`, module cache uses `golang.org/x/sys@v0.0.0-20220811…`,
`golang.org/x/sync@v0.0.0-20210220…`. MQTT user **`gateway`** (constrained ACL,
see [`../docs/mqtt-bus.md`](../docs/mqtt-bus.md)).

## Role

`gateway` is the **only off-box-reaching service** on the cloud side: it bridges
the internal loopback **MQTT** bus to **AWS IoT Core** over mutual TLS, carrying:

- **Device Shadow** (`configshadow`, `telemetry-config`, "set initial telemetry
  config") — cloud config ↔ bike state.
- **Telemetry** ("Publishing telemetry", "Publishing batch") — CBOR-encoded
  metrics to the cloud.
- **AWS IoT Jobs** ("Executing job", "No matching job handler found",
  `handleNotifyNext`, `update/accepted`, `update/rejected`, statuses
  `IN_PROGRESS`/`SUCCEEDED`/`FAILED`/`REJECTED`/`TerminalStateReached`) — the
  cloud-driven OTA/command trigger that feeds `update`/`power state set` etc.
- **Bike-id sync** ("Reporting bike id", "Updating bike id", "check bike id",
  `/run/media/mmcblk2p6/bike_id`, "Bike id is in sync").
- **Transport selection** between the **modem** (LTE-M) and the **BLE proxy**
  ("Prefer LTE-M modem", "Prefer BLE proxy", "Only BLE proxy", "prefer-modem"/
  "prefer-proxy", "auto-disconnect", `modem/info/network`, `ble/proxy/config`).

The MQTT stack is **Eclipse Paho `paho.golang` (MQTT v5)** — the property set is
present (`ReasonString`, `TopicAlias`, `MessageExpiry`, `PayloadFormat`,
`CorrelationData`, `ResponseTopic`, `session_present`, full CONNECT/CONNACK/
SUB/PUB/PUBREC/PUBREL/PUBCOMP/DISCONNECT handling). Payloads are **CBOR**
(`fxamacker/cbor`-style: "decode cbor", "encode cbor", "cbor: …" diagnostics).

## TLS / certificate posture (security)

**The connection to AWS IoT is mutual-TLS with a pinned trust anchor.**

- **Server trust = root-CA pinning.** The binary embeds **two Amazon Root CAs**
  and builds a *custom* `x509.CertPool` from **only** those two (`FUN_001ca560`:
  `NewCertPool` → `AppendCertsFromPEM` ×2, **panics if either append fails**), then
  hands that pool to the `tls.Config` it builds (`FUN_002b0c20`, which also sets
  `MinVersion = 0x0303` = **TLS 1.2**). It does **not** fall back to the system
  trust store. The two roots are:

  | Loc | Cert | Subject |
  | --- | --- | --- |
  | `0x663c80` | RSA 2048 (`MIIDQTCC…`) | **Amazon Root CA 1** |
  | `0x65d860` | ECDSA P-256 (`MIIBtjCC…`) | **Amazon Root CA 3** |

  (Only CA 3 is a *defined* string; CA 1 is an undefined byte blob, so a plain
  string scan misses it — found via `search_byte_patterns`.)

- **So is certificate pinning active? Yes — at the CA/root level.** The AWS IoT
  server cert must chain to one of these two hardcoded Amazon roots; trusting any
  other CA is impossible. This is the **AWS-recommended posture** and is stricter
  than using the OS CA bundle. It is **not** leaf / public-key (SPKI) pinning —
  there's no `VerifyPeerCertificate`/`VerifyConnection` callback comparing a fixed
  server cert/key, and **no `InsecureSkipVerify`** (verification is enforced — a
  custom `RootCAs` pool would be pointless otherwise).

- **Client auth = mTLS device cert.** The bike presents its own client
  certificate + key loaded from disk at runtime (`certificate.pem`, "read
  certificate: %w", "read private key: %w", "getCert can't be nil") — the
  per-device identity AWS IoT authenticates.

- **Endpoint is not hardcoded.** There is **no `*.amazonaws.com` literal** in the
  binary; the AWS IoT endpoint is read from the provisioning config file (below).

## Provisioning & config (`/run/media/mmcblk2p6/`)

The device's cloud identity + config live on the **persistent eMMC config
partition `mmcblk2p6`** (the same one that holds `bike_id` and the mosquitto
persistence). `provisioning_load_from_mmcblk2p6` (`0x1c6bb0`) reads five files
from `/run/media/mmcblk2p6/`:

| File | Purpose |
| --- | --- |
| **`config.cfg`** | **the config file the AWS IoT endpoint is read from** — a `peterbourgon/ff` plain `key value` file; keys `external-endpoint` / `iot_endpoint` (+ `prefer-modem`/`prefer-proxy`/`auto-disconnect`, …). Parsed by `config_cfg_parse` (`0x1c70d0`); wraps `open config: %w` / `read endpoint: %w`. Optional (`allowMissingConfigFile`) — flags/env can override. |
| `certificate.pem` | the **mTLS client certificate** presented to AWS IoT |
| `private.key` | the client **private key** |
| `serial` | device serial |
| `bike_id` | bike identifier (synced to/from the cloud) |

So the AWS endpoint (and the whole IoT identity) is **provisioned per-device onto
`mmcblk2p6`**, not baked into the binary — found by xref'ing the interior string
pointers (the Go symbol pass doesn't bind function names for this go1.19 build, so
key functions were identified + renamed by hand: `iot_ca_CertPool_amazon_roots`
`0x1ca560`, `iot_build_tls_config` `0x2b0c20`, `config_cfg_parse` `0x1c70d0`,
`provisioning_load_from_mmcblk2p6` `0x1c6bb0`).

## "Long numeric string" — identified

The very long pure-digit strings in the binary (e.g.
`11368683772161602973937988281255684341886080801486968994140625`,
`28421709430404007434844970703125`, `5684341886080801486968994140625`, …) are
**consecutive powers of 5** — Go's `strconv` exact decimal-expansion constant
table used for correct round-trip `float ↔ string` conversion (each is the exact
decimal of a negative power of two). **Standard Go runtime data — not VanMoof,
not a key/ID/secret.** (The only VanMoof-relevant long-ish identifiers are the Go
module *pseudo-versions* in the build paths, e.g. `…@v0.0.0-20220811171246-…`.)

## AWS IoT Jobs & Shadow handlers

Recovered from `gopclntab` symbol names (the names are present even though the
analyzer won't bind them to the `FUN_*` bodies):

**Jobs** — `internal/iot/job` (the cloud → bike command channel,
`$aws/things/<thing>/jobs/…`):

| Method | Role |
| --- | --- |
| `job.Client.getPendingExecutions` (`SetClientToken`) | ask AWS for queued jobs (`get-pending`) |
| `job.Client.handleNotifyNext` (`.func1`) | the **notify-next** subscriber — fires when a new job is pushed; `unmarshalling notifyNextPayload` → dispatches it |
| `job.Client.execute` (`0x2b34d0`) | run the job (the dispatch — see below) |
| `job.Client.updateStatus` | report progress on `…/jobs/<id>/update` — `IN_PROGRESS` → `SUCCEEDED` / `FAILED` / `REJECTED`, `TerminalStateReached` |
| `job.parseJobErrorResponse`, `job.document.Context` | error parse + per-job context |

Job results are also mirrored to the local bus as `update/accepted` / `update/rejected`.

#### What commands can the cloud actually push? — **exactly one: `log_upload`**

`job.Client.execute` (`0x2b34d0`) reads the job document's **`type`** field (`0x2d04fe`;
the sibling **`job_id`** key is at `0x2d0afa`) and looks it up in a
**`map[string]handler`** (map type `0x44b520`). On a **miss** it
logs **`No matching job handler found.`** (`0x2db32c`) and sets the execution
**`REJECTED`**; on a **hit** it goes **`IN_PROGRESS`** → runs the handler → **`SUCCEEDED`**
or **`FAILED`**. The handler map is populated in exactly **one** place
(`gateway_construct_register_jobhandlers` `0x2b6680`, a single `mapassign_faststr`) with
exactly **one** key — **`log_upload`** (`0x2d214a`). So the *entire* cloud → bike Jobs
command surface is a single operation; any other `type` is rejected unhandled.

`job_handler_log_upload` (`0x2b8310`) is a **diagnostic log-exfiltration-on-demand**
routine, **not** an OTA or arbitrary-command channel:

1. The job document must carry an **upload URL** — absent ⇒ `missing URL` (`0x2d27ac`).
2. It takes a **time range**, **capped at 24 h** (`86400000000000` ns) — over that ⇒
   `log collection range too large, max %s allowed` (`0x2e26a9`).
3. It collects logs (`journalctl` + the `logs` set, `0x2d047e`) into a **temp file**
   (`create temp log file: %w`, `sync log file to disk: %w`, `collect logs: %w`), then
4. **HTTP-uploads** that file to the (presigned) URL, expecting **HTTP 200** — otherwise
   `unsuccessful response: status %d, body :%s` (`0x2e13b1`).

So OTA is **not** triggered through Jobs; the OTA/update path is the `update` service
fed by the local bus + shadow, and the `update/accepted`/`update/rejected` topics are the
*local* mirror of job status, not a second Jobs handler.

**Shadow** — `internal/iot/shadow` (`$aws/things/<thing>/shadow`, classic shadow):

| Method | Role |
| --- | --- |
| `shadow.Client.Report` (`.func1/2`), `.State`, `.topic`, `.clientToken` | publish reported state / read desired state |
| `shadow.Document.{ParseDocument, Field, SetField, JSON, Empty}`, `State.{Marshal,Unmarshal}JSON` | the shadow doc model |
| `internal/gateway/configshadow` | the **config delivered via the shadow** (`telemetry-config`, `set initial telemetry config`) — runtime config on top of the bootstrap `config.cfg` |

So the cloud control plane is: **Jobs** push exactly one imperative command
(`log_upload` — see above), the **Shadow** carries declarative config/state (and is
where OTA intent actually lands), and **telemetry** batches metrics to `$aws/rules/…`.

## Next

- [x] Enumerate the registered **job operation handlers** — done: the handler map
      (`0x44b520`) holds a single key, **`log_upload`** (`job_handler_log_upload`
      `0x2b8310`); everything else is `REJECTED` "No matching job handler found".
- [x] Confirm the MQTT-bus ↔ AWS topic mapping — done: the binary-confirmed
      bridge table is in [`../docs/mqtt-bus.md`](../docs/mqtt-bus.md) ("Gateway ↔
      AWS IoT bridge"). bus→cloud = telemetry (CBOR) + shadow report + bike_id;
      cloud→bus = shadow-desired telemetry-config/bike_id + the `log_upload` job;
      the `…/set` command writes are ACL-granted but not exercised by the gateway.

## Reconstruction (`src/`)

The full VanMoof service is reconstructed as readable Go under
[`src/`](src/) — all 10 `internal/*` packages (~3.4k lines, gofmt-clean), one
file per OEM `.go`, every function tagged with its `// OEM 0x…` address. See
[`progress.md`](progress.md) for the per-package status, inventory and the
honesty flags. It **compiles and vets clean** (`go build ./...` + `go vet ./...`
exit 0) against the real pinned vendor deps (`paho.golang v0.23.0`, `cbor`, `zap`,
`ff`, `gjson`); `gateway.New` wires real objects via reconstructed constructors
(the OEM inlined them). Behaviour-faithful, not bit-exact, untested on hardware.

> **Ghidra note — Go symbol recovery:** the Golang Symbols analyzer runs but does
> **not** bind names for this go1.19 build (functions stay `FUN_*`). The fix: the
> binary is stripped of `.symtab` but the **`gopclntab`** (`.data.rel.ro.gopclntab`
> @ `0x4af8e0`, magic `0xfffffff0`) carries every name. Parsing it off the ELF
> (`s5_decomp_tmp/parse_pclntab.py`) yields the complete name↔address map — 6305
> funcs, `textStart 0x11000`; the 163 VanMoof functions are in `progress.md`. Key
> detail: a functab `funcoff` is relative to the **functab base** (pcHeader +
> `pclnOffset`), not the pcHeader. With the map, any function decompiles by address
> (`mcp__ghidra__decompile_function program=gateway address=<addr>`) and callees
> resolve by grepping the map. (`go tool nm` does **not** work — it needs a real
> `.symtab` and won't fall back to the pclntab.)
