# `gateway/src` — reconstructed Go

Behaviour-faithful **Go** reconstruction of the VanMoof gateway service. The
module is the OEM path `github.com/VanMoof/embedded/gateway` (`go.mod`): the
`main` package (the ffcli entry point) is at the module root
(`main.go`/`run.go`/`version.go`) and the libraries are under `internal/<pkg>/`,
one file per OEM `.go` source where known. See [`../progress.md`](../progress.md)
for the function inventory + addresses and [`../README.md`](../README.md) for the
service overview.

The module **compiles and vets clean** against the real, pinned vendor deps
(`go build ./...` and `go vet ./...` both exit 0; `go.mod` pins
`eclipse/paho.golang v0.23.0`, `fxamacker/cbor/v2`, `peterbourgon/ff/v3`,
`tidwall/gjson`+`sjson`, `uber/zap`). `gofmt`-clean throughout. `gateway.New`
wires real objects (no stubs) through reconstructed constructors
(`mqtt.NewClient`, `telemetry.NewCollector`/`NewRouter`, `shadow.NewClient`,
`job.NewClient`, `ble.NewProxy`) — these were inlined in the OEM binary, so they
were rebuilt from `gateway.New` (`0x2b6680`) / `iot.NewClient` (`0x2b0c20`). It is
behaviour-faithful but not bit-exact, and not runtime-tested against a real bike.

## Conventions

- Reconstruct **only VanMoof `internal/*` code**. Vendor packages (Go runtime,
  `github.com/eclipse/paho.golang/paho`, `github.com/fxamacker/cbor/v2`,
  `go.uber.org/zap`, `github.com/peterbourgon/ff`, `github.com/tidwall/gjson`,
  `crypto/tls`, `crypto/x509`, `net/http`, `os/exec`, …) are **imported and
  modelled**, never reconstructed.
- Each function carries a one-line comment with its **OEM entry address**
  (`// OEM 0x2b34d0`), so the Go maps back to the binary.
- Faithful behaviour, not byte-identical: clean idiomatic Go, real control flow,
  the actual topic/format-string literals, struct fields named from their use.
- **No AI references anywhere.** No build is expected (vendor deps not vendored);
  this is a reading reconstruction, same bar as the C/C++ targets.

## Method

The binary is stripped but the **gopclntab carries every symbol**. The full
name↔address map was recovered to `s5_decomp_tmp/gateway_syms.txt` (all 6305
funcs) and `s5_decomp_tmp/vanmoof_funcs.txt` (the 163 VanMoof funcs). To read a
function: `mcp__ghidra__decompile_function program=gateway address=<addr>`.
Callees show as `FUN_xxxxxxxx`; resolve each by grepping the map
(`/usr/bin/grep -i <addr-without-0x> gateway_syms.txt`) — that tells you whether
it is a VanMoof call or a vendor/runtime call to model.
