// Package ble provides the BLE proxy transport: a telemetry sink that
// republishes batches onto the local bus for the BLE side to relay, used as a
// fallback when the LTE-M modem link is unavailable.
package ble

import (
	"fmt"

	"go.uber.org/zap"
)

// proxyTopic is the local-bus topic the BLE relay listens on. A batch
// published here is picked up by the BLE service and forwarded to a connected
// phone, which carries it to the cloud on the gateway's behalf.
const proxyTopic = "ble/proxy"

// Batch is the telemetry batch the proxy forwards. It is
// internal/telemetry.Batch; only its CBOR marshalling is used here.
type Batch interface {
	MarshalBinary() ([]byte, error)
}

// Publisher is the local MQTT bus the proxy republishes to (internal/mqtt
// .Client). topic/payload/qos/retain map straight onto an MQTT publish.
type Publisher interface {
	Publish(topic string, payload []byte, qos byte, retain bool) error
}

// Proxy republishes telemetry batches onto the BLE relay topic. SetConnected
// gates publishing on whether the BLE side is actually attached.
type Proxy struct {
	pub       Publisher
	log       *zap.Logger
	connected bool // field at OEM offset +0x18
}

// NewProxy builds a Proxy over the given bus.
func NewProxy(pub Publisher, log *zap.Logger) *Proxy {
	return &Proxy{pub: pub, log: log}
}

// SetConnected records whether the BLE side is currently connected. While it is
// not, Publish is a silent no-op. (The OEM has no standalone symbol for this —
// it is the trivial setter of the connected flag, inlined at its call site in
// the gateway's proxy-config handler.)
//
// OEM (inlined — sets Proxy.connected at +0x18)
func (p *Proxy) SetConnected(connected bool) {
	p.connected = connected
}

// Publish forwards batch to the BLE relay. If the BLE side is not connected the
// call returns nil immediately (the batch is simply dropped — the modem path is
// expected to carry it). Otherwise the batch is CBOR-marshalled and published
// retained to ble/proxy at QoS 1.
//
// OEM 0x2b2210
func (p *Proxy) Publish(batch Batch) error {
	if !p.connected {
		return nil
	}

	raw, err := batch.MarshalBinary()
	if err != nil {
		return fmt.Errorf("marshal batch: %w", err)
	}

	payload, err := cborMarshal(raw)
	if err != nil {
		return fmt.Errorf("marshal cbor: %w", err)
	}

	p.log.Info("Publishing telemetry")
	return p.pub.Publish(proxyTopic, payload, 1, true)
}
