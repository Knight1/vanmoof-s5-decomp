// Package telemetry implements the bus->cloud metrics path of the gateway:
// a Collector that subscribes to local-bus topics, buffers and CBOR-marshals a
// Batch, and a Router that ships the batch to AWS IoT over the LTE-M modem or
// the BLE proxy depending on the configured TransportMode.
//
// OEM source: github.com/VanMoof/embedded/gateway/internal/telemetry
// (batch.go, collector.go, config.go, ignore.go, mode.go, router.go)
package telemetry

import "fmt"

// TransportMode selects which physical link the Router uses to publish a batch
// to the cloud. It is delivered through the device shadow telemetry-config and
// can be changed at runtime via (*Router).SetTransportMode.
//
// OEM source: mode.go
type TransportMode int

const (
	// PreferProxy publishes over the BLE proxy first and falls back to the
	// LTE-M modem if the proxy publish fails.
	PreferProxy TransportMode = iota // 0
	// PreferModem publishes over the LTE-M modem first and falls back to the
	// BLE proxy if the modem publish fails.
	PreferModem // 1
	// OnlyProxy publishes over the BLE proxy only.
	OnlyProxy // 2
	// OnlyModem publishes over the LTE-M modem only.
	OnlyModem // 3
	// Both publishes over both links and joins the resulting errors.
	Both // 4
)

// String returns the canonical CamelCase name of the mode.
//
// OEM 0x2af490
func (m TransportMode) String() string {
	switch m {
	case PreferProxy:
		return "PreferProxy"
	case PreferModem:
		return "PreferModem"
	case OnlyProxy:
		return "OnlyProxy"
	case OnlyModem:
		return "OnlyModem"
	case Both:
		return "Both"
	default:
		return "Unknown"
	}
}

// MarshalText renders the mode as the kebab-case token used in the shadow
// config. It errors for an out-of-range mode.
//
// OEM 0x2af520
func (m TransportMode) MarshalText() ([]byte, error) {
	switch m {
	case PreferProxy:
		return []byte("prefer-proxy"), nil
	case PreferModem:
		return []byte("prefer-modem"), nil
	case OnlyProxy:
		return []byte("only-proxy"), nil
	case OnlyModem:
		return []byte("only-modem"), nil
	case Both:
		return []byte("both"), nil
	default:
		return nil, fmt.Errorf("unknown mode: %d", int(m))
	}
}

// UnmarshalText parses a mode token. Several spellings are accepted for each
// mode; unknown tokens error with "invalid mode %q".
//
// OEM 0x2af710
func (m *TransportMode) UnmarshalText(text []byte) error {
	switch string(text) {
	case "prefer-modem", "prefer_modem", "modem-first":
		*m = PreferModem
	case "prefer-proxy", "prefer_proxy", "proxy-first":
		*m = PreferProxy
	case "modem", "modem-only", "modem_only", "modemonly",
		"only-modem", "only_modem", "onlymodem":
		*m = OnlyModem
	case "proxy", "proxy-only", "proxy_only", "proxyonly",
		"only-proxy", "only_proxy", "onlyproxy":
		*m = OnlyProxy
	case "all", "both":
		*m = Both
	default:
		return fmt.Errorf("invalid mode %q", string(text))
	}
	return nil
}
