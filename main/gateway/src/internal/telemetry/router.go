package telemetry

import (
	"fmt"
	"sync"
)

// Transport is one publish link (the LTE-M modem-backed iot.Client, or the
// BLE ble.Proxy). Both satisfy this single-method interface.
//
// OEM source: router.go (telemetry.Transport)
type Transport interface {
	// Publish sends the (already CBOR+zlib encoded) payload to topic.
	Publish(topic string, payload []byte) error
}

// Router fans a published batch out over the configured transport(s) according
// to its TransportMode. modem is the LTE-M link, proxy is the BLE link.
//
// OEM source: router.go (telemetry.Router)
type Router struct {
	mu    sync.Mutex
	mode  TransportMode
	modem Transport
	proxy Transport
}

// NewRouter builds a Router. The initial mode is the zero value (PreferProxy)
// unless SetTransportMode is called.
//
// OEM source: router.go (telemetry.NewRouter)
func NewRouter(modem, proxy Transport) *Router {
	return &Router{modem: modem, proxy: proxy}
}

// SetTransportMode atomically swaps the active mode.
//
// OEM 0x2afbe0
func (r *Router) SetTransportMode(mode TransportMode) {
	r.mu.Lock()
	r.mode = mode
	r.mu.Unlock()
}

// Publish ships the payload to topic over the link(s) selected by the current
// mode:
//
//   - PreferProxy: try proxy, on error fall back to modem; errors wrapped
//     "proxy: %w" / "fallback to modem: %w".
//   - PreferModem: try modem, on error fall back to proxy; errors wrapped
//     "modem: %w" / "fallback to proxy: %w".
//   - OnlyProxy:   proxy only          ("proxy: %w").
//   - OnlyModem:   modem only          ("modem: %w").
//   - Both:        both links, errors joined ("proxy: %w", "modem: %w").
//
// OEM 0x2afcd0
func (r *Router) Publish(topic string, payload []byte) error {
	r.mu.Lock()
	mode := r.mode
	r.mu.Unlock()

	switch mode {
	case PreferProxy:
		errProxy := r.proxy.Publish(topic, payload)
		if errProxy == nil {
			return nil
		}
		errModem := r.modem.Publish(topic, payload)
		if errModem == nil {
			return nil
		}
		return fmt.Errorf("proxy: %w: fallback to modem: %w", errProxy, errModem)

	case PreferModem:
		errModem := r.modem.Publish(topic, payload)
		if errModem == nil {
			return nil
		}
		errProxy := r.proxy.Publish(topic, payload)
		if errProxy == nil {
			return nil
		}
		return fmt.Errorf("modem: %w: fallback to proxy: %w", errModem, errProxy)

	case OnlyProxy:
		if err := r.proxy.Publish(topic, payload); err != nil {
			return fmt.Errorf("proxy: %w", err)
		}
		return nil

	case OnlyModem:
		if err := r.modem.Publish(topic, payload); err != nil {
			return fmt.Errorf("modem: %w", err)
		}
		return nil

	case Both:
		errProxy := r.proxy.Publish(topic, payload)
		errModem := r.modem.Publish(topic, payload)
		if errProxy != nil && errModem != nil {
			return errorsJoin(
				fmt.Errorf("proxy: %w", errProxy),
				fmt.Errorf("modem: %w", errModem),
			)
		}
		return nil

	default:
		return fmt.Errorf("unknown mode: %v", mode)
	}
}

// errorsJoin mirrors the errors.Join call the OEM uses to combine the two
// link errors in Both mode (FUN_001c6160, errors.Join of 2).
func errorsJoin(errs ...error) error {
	// Modelled: real build links the stdlib errors.Join.
	var joined error
	for _, e := range errs {
		if e == nil {
			continue
		}
		if joined == nil {
			joined = e
		} else {
			joined = fmt.Errorf("%w\n%w", joined, e)
		}
	}
	return joined
}
