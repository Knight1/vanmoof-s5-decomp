// Package event wires the local loopback MQTT bus to the gateway/telemetry
// state. Each Handle* function subscribes to one bus topic and, for every
// message, decodes the CBOR payload and pushes the result into the gateway
// (BLE auth events, transport-mode changes, the modem's network/timezone
// info).
package event

import (
	"fmt"
	"strings"
	"time"
)

// Bus is the subset of the internal MQTT client the event handlers use: a
// topic-pattern Subscribe whose callback receives each message's topic and
// raw (CBOR) payload. It is satisfied by internal/mqtt.Client.
type Bus interface {
	Subscribe(topic string, handler func(topic string, payload []byte)) error
}

// bleConnectionsTopic matches every BLE connection-handle event. The trailing
// "+" is a single-level MQTT wildcard capturing the connection handle id; the
// prefix below (without the wildcard) is stripped off to recover it.
const (
	bleConnectionsTopic  = "ble/connections/handle/+"
	bleConnectionsPrefix = "ble/connections/handle/" // 23 bytes
)

// goEpochOffset is the number of seconds between the proleptic-Gregorian year-1
// epoch used by the payload timestamp and the Unix epoch (0xe7791f700).
const goEpochOffset = 62135596800

// blePayload is the CBOR body of a ble/connections/handle/<id> message. Status
// is either "authenticated" (the phone completed VanMoof's BLE challenge) or
// "invalid". For the authenticated case the payload also carries the absolute
// time at which authentication happened.
type blePayload struct {
	Status    string `cbor:"status"`
	Timestamp int64  `cbor:"timestamp"`
}

// BLEAuth receives a decoded BLE authentication event. handle is the
// connection id taken from the topic; the authenticated event carries the time
// at which the phone passed the challenge.
type BLEAuth interface {
	SetBLEAuthenticated(handle string, at time.Time)
	SetBLEInvalid(handle string)
}

// HandleBLE subscribes to ble/connections/handle/+ and forwards each
// connection's authentication result to dst.
//
// OEM 0x294260
func HandleBLE(bus Bus, dst BLEAuth) error {
	err := bus.Subscribe(bleConnectionsTopic, func(topic string, payload []byte) {
		// OEM 0x2945a0  (HandleBLE.func1 — the per-message callback)
		var msg blePayload
		if err := cborUnmarshal(payload, &msg); err != nil {
			logf("unmarshal: %v\n", err)
			return
		}

		// The connection handle is the topic segment after the fixed prefix.
		handle := strings.TrimPrefix(topic, bleConnectionsPrefix)

		switch msg.Status {
		case "authenticated":
			// The payload timestamp is an absolute count whose epoch is
			// year 1; add the Go→Unix epoch offset to build wall-clock time.
			at := time.Unix(msg.Timestamp+goEpochOffset, 0)
			dst.SetBLEAuthenticated(handle, at)
		case "invalid":
			dst.SetBLEInvalid(handle)
		default:
			// Unknown status — ignore.
		}
	})
	if err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}
	return nil
}
