package event

import "fmt"

// proxyConfigTopic carries the transport-preference config from the BLE side:
// which link telemetry should travel over (LTE-M modem, BLE proxy, or both).
const proxyConfigTopic = "ble/proxy/config"

// TransportMode mirrors telemetry.TransportMode. The numeric values match the
// order the OEM assigns when decoding the "mode" string.
type TransportMode int

const (
	transportBLE   TransportMode = 1 // "ble"   — proxy only
	transportModem TransportMode = 2 // "modem" — LTE-M only
	transportBoth  TransportMode = 3 // "both"  — either link
)

// proxyConfigPayload is the CBOR body of a ble/proxy/config message.
type proxyConfigPayload struct {
	Mode string `cbor:"mode"`
}

// HandleProxyConfig subscribes to ble/proxy/config and, for each message,
// decodes the requested transport mode and calls setMode. Messages whose mode
// string is none of "ble"/"modem"/"both" are ignored.
//
// OEM 0x294950
func HandleProxyConfig(bus Bus, setMode func(TransportMode)) error {
	err := bus.Subscribe(proxyConfigTopic, func(_ string, payload []byte) {
		// OEM 0x294c50  (HandleProxyConfig.func1)
		var msg proxyConfigPayload
		if err := cborUnmarshal(payload, &msg); err != nil {
			logf("unmarshal: %v\n", err)
			return
		}

		var mode TransportMode
		switch msg.Mode {
		case "ble":
			mode = transportBLE
		case "modem":
			mode = transportModem
		case "both":
			mode = transportBoth
		default:
			// Unknown mode — ignore.
			return
		}
		setMode(mode)
	})
	if err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}
	return nil
}
