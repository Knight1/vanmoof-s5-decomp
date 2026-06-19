package telemetry

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"io"
	"time"

	"github.com/fxamacker/cbor/v2"
)

// Attributes are the labels and the numeric value carried by a single
// measurement sample. Labels qualify the value (e.g. unit, channel); Value is
// the reading itself.
//
// OEM source: batch.go (telemetry.Attributes)
type Attributes struct {
	Value  float64           `cbor:"value"`
	Labels map[string]string `cbor:"labels"`
}

// Message is one buffered bus message: the source topic, its decoded CBOR
// payload, the time it was received, and the attributes extracted from it.
//
// OEM source: batch.go (telemetry.message / telemetry.Message)
type Message struct {
	Topic      string
	Timestamp  time.Time
	Attributes Attributes
	Data       []byte
}

// Batch is the in-memory accumulation a Collector flushes to the cloud. It
// records the firmware/ECU versions, the active timezone offset and BLE user in
// effect for the samples, and the buffered messages keyed by topic.
//
// OEM source: batch.go (telemetry.Batch)
type Batch struct {
	Firmware string
	ECU      string
	Timezone string
	User     string
	Messages map[string][]Message
}

// wireBatch is the CBOR representation that goes on the wire. Field order and
// tags are reproduced verbatim from the image:
//
//	Batch  -> {msgs, fw, ecu, tz?, u?}
//	sample -> [dt, data, value, labels]  (",toarray")
//
// The per-message timestamp is encoded as Unix epoch milliseconds (the OEM
// arithmetic value/1e6 + sec*1e3 - 62135596800000 converts Go absolute time to
// Unix ms).
//
// OEM source: batch.go (telemetry.wireBatch)
type wireBatch struct {
	Messages map[string][]wireMessage `cbor:"msgs"`
	Firmware string                   `cbor:"fw"`
	ECU      string                   `cbor:"ecu"`
	Timezone string                   `cbor:"tz,omitempty"`
	User     string                   `cbor:"u,omitempty"`
}

// wireMessage is encoded as a CBOR array (",toarray"); the cbor:"dt"/"data"
// field tags seen in the image are vestigial under toarray, where positional
// order is what matters: [dt, data, value, labels].
type wireMessage struct {
	_      struct{}          `cbor:",toarray"`
	DT     int64             // cbor:"dt"  — Unix epoch milliseconds
	Data   []byte            // cbor:"data"
	Value  float64           // cbor:"value"
	Labels map[string]string // cbor:"labels"
}

// MarshalBinary CBOR-encodes the batch and zlib-compresses the result. Encode
// failures are wrapped "encode cbor: %w"; compressor failures propagate.
//
// OEM 0x2ab440
func (b Batch) MarshalBinary() ([]byte, error) {
	wb := wireBatch{
		Messages: make(map[string][]wireMessage, len(b.Messages)),
		Firmware: b.Firmware,
		ECU:      b.ECU,
		Timezone: b.Timezone,
		User:     b.User,
	}
	for topic, msgs := range b.Messages {
		out := make([]wireMessage, len(msgs))
		for i, m := range msgs {
			out[i] = wireMessage{
				// OEM: ns/1e6 + sec*1e3 - 62135596800000 == Unix epoch ms.
				DT:     m.Timestamp.UnixMilli(),
				Data:   m.Data,
				Value:  m.Attributes.Value,
				Labels: m.Attributes.Labels,
			}
		}
		wb.Messages[topic] = out
	}

	encoded, err := cbor.Marshal(wb)
	if err != nil {
		return nil, fmt.Errorf("encode cbor: %w", err)
	}

	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	if _, err := w.Write(encoded); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// UnmarshalBinary reverses MarshalBinary: zlib-inflate then CBOR-decode. A
// decompressor construction failure is wrapped "create zlib reader: %w" and a
// decode failure "decode cbor: %w".
//
// OEM 0x2ab910
func (b *Batch) UnmarshalBinary(data []byte) error {
	zr, err := zlib.NewReader(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("create zlib reader: %w", err)
	}
	raw, err := io.ReadAll(zr)
	if err != nil {
		return err
	}

	var wb wireBatch
	if err := cbor.Unmarshal(raw, &wb); err != nil {
		return fmt.Errorf("decode cbor: %w", err)
	}

	b.Firmware = wb.Firmware
	b.ECU = wb.ECU
	b.Timezone = wb.Timezone
	b.User = wb.User
	b.Messages = make(map[string][]Message, len(wb.Messages))
	for topic, msgs := range wb.Messages {
		out := make([]Message, len(msgs))
		for i, m := range msgs {
			sec := m.DT / 1000
			nsec := (m.DT % 1000) * 1e6
			out[i] = Message{
				Topic:     topic,
				Timestamp: time.Unix(sec, nsec).UTC(),
				Data:      m.Data,
				Attributes: Attributes{
					Value:  m.Value,
					Labels: m.Labels,
				},
			}
		}
		b.Messages[topic] = out
	}
	return nil
}
