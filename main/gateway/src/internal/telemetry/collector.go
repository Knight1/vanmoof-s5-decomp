package telemetry

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/eclipse/paho.golang/paho"
	"github.com/fxamacker/cbor/v2"
	"go.uber.org/zap"

	"github.com/VanMoof/embedded/gateway/internal/mqtt"
)

// publisher is the cloud-publish dependency of the Collector. It is satisfied
// by *Router; the batch is shipped to the AWS topic "telemetry/<bike-id>".
//
// OEM source: collector.go (telemetry.Publisher)
type publisher interface {
	Publish(topic string, payload []byte) error
}

// retentionDefault is the maximum age a buffered sample may reach before
// dropOld discards it (24h). It is the value used when a publish fails and the
// config supplies no max_age_seconds override.
//
// OEM constant: 86400000000000 ns (flush.go / dropOld).
const retentionDefault = 24 * time.Hour

// Collector subscribes to the local MQTT bus, buffers decoded messages per
// topic, and periodically flushes them as one CBOR Batch to the cloud via the
// publisher. All mutable state is guarded by mu.
//
// OEM source: collector.go (telemetry.Collector)
type Collector struct {
	log    *zap.Logger
	bus    *mqtt.Client // local-bus subscriber (mqtt://localhost:1883)
	pub    publisher    // the Router, publishing to AWS IoT
	bikeID string       // AWS publish topic is "telemetry/<bikeID>"

	mu sync.Mutex

	// metadata attached to every batch
	firmware string
	ecu      string
	timezone string // signed UTC offset string
	user     string // current BLE user UUID (uuidp formatted)

	// per-topic subscriptions currently active on the bus
	topics []string

	// buffered messages keyed by topic, and the pending flush timer
	messages map[string][]Message
	interval time.Duration
	timer    *time.Timer
}

// NewCollector builds a Collector bound to the local-bus client and the cloud
// publisher (the Router). bikeID keys the cloud publish topic
// "telemetry/<bikeID>". The message buffer is created lazily on the first
// handled message, so messages/timer start nil.
//
// OEM source: collector.go (inlined into gateway.New @0x2b6680).
func NewCollector(log *zap.Logger, bus *mqtt.Client, pub publisher, bikeID string) *Collector {
	return &Collector{
		log:    log,
		bus:    bus,
		pub:    pub,
		bikeID: bikeID,
	}
}

// SetConfig applies a new telemetry Config: it (re)computes the bus
// subscriptions and stores the retention/interval settings. Subscribe failures
// are wrapped "set subscriptions: %w".
//
// OEM 0x2ac0d0
func (c *Collector) SetConfig(ctx context.Context, cfg Config) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := c.setSubscriptions(ctx, cfg.Subscriptions); err != nil {
		return fmt.Errorf("set subscriptions: %w", err)
	}
	c.interval = time.Duration(cfg.MaxAge) * time.Second
	return nil
}

// setSubscriptions diffs the requested topic set against the active one and
// issues the corresponding bus Subscribe / Unsubscribe calls, logging
// "Subscribing to topics" and "Unsubscribing from topics". Must be called with
// mu held.
//
// OEM 0x2ac580
func (c *Collector) setSubscriptions(ctx context.Context, subs []Subscription) error {
	want := make([]string, 0, len(subs))
	for _, s := range subs {
		if !contains(want, s.Topic) {
			want = append(want, s.Topic)
		}
	}

	add := diff(want, c.topics)    // in want, not currently subscribed
	remove := diff(c.topics, want) // currently subscribed, no longer wanted

	if len(add) > 0 {
		c.log.Debug("Subscribing to topics", zap.Strings("topics", add))
		subscriptions := make([]mqtt.Subscription, len(add))
		for i, topic := range add {
			subscriptions[i] = mqtt.Subscription{Topic: topic, Callback: c.handleMessage}
		}
		if err := c.bus.Subscribe(ctx, subscriptions); err != nil {
			return fmt.Errorf("subscribe: %w", err)
		}
	}
	if len(remove) > 0 {
		c.log.Debug("Unsubscribing from topics", zap.Strings("topics", remove))
		if err := c.bus.Unsubscribe(ctx, remove); err != nil {
			return fmt.Errorf("unsubscribe: %w", err)
		}
	}

	c.topics = want
	return nil
}

// SetFirmware records the firmware version stamped on every batch.
//
// OEM 0x2ace30
func (c *Collector) SetFirmware(version string) {
	c.mu.Lock()
	c.firmware = version
	c.mu.Unlock()
}

// SetECU records the ECU version stamped on every batch.
//
// OEM 0x2acfd0
func (c *Collector) SetECU(version string) {
	c.mu.Lock()
	c.ecu = version
	c.mu.Unlock()
}

// SetTimezone records the active UTC offset. When the offset changes, the
// currently buffered samples (collected under the previous offset) are flushed
// first so they are not relabelled. A flush error is logged "Failed to flush
// with previous timezone" but does not block the update.
//
// OEM 0x2ad170
func (c *Collector) SetTimezone(offset string) {
	c.mu.Lock()
	defer c.mu.Unlock()

	switch {
	case c.timezone == "":
		c.log.Debug("Detected timezone offset", zap.String("offset", offset))
	case c.timezone != offset:
		c.log.Debug("Detected timezone offset",
			zap.String("new_offset", offset),
			zap.String("old_offset", c.timezone))
		if err := c.flush(); err != nil {
			c.log.Error("Failed to flush with previous timezone", zap.Error(err))
		}
	}
	c.timezone = offset
}

// SetBLE records the connected BLE user. When the user changes, buffered
// samples are flushed first (so they keep the prior user's attribution); a
// flush error is logged "Failed to flush with previous user". The user is
// formatted from its 16-byte UUID by uuidp.
//
// OEM 0x2ad710
func (c *Collector) SetBLE(uuid []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()

	user := uuidp(uuid)
	if c.user != user {
		if err := c.flush(); err != nil {
			c.log.Error("Failed to flush with previous user", zap.Error(err))
		}
		c.user = user
	}
}

// Flush publishes any buffered messages immediately, taking the lock.
//
// OEM 0x2adc40
func (c *Collector) Flush() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.flush()
}

// flush builds and publishes a Batch from the buffered messages. It first
// cancels any pending flush timer; with nothing buffered it returns nil.
// "Publishing batch" is logged before the publish. On success the buffer is
// cleared and "Batch sent" logged with the elapsed duration. On failure the
// error is wrapped "publish: %w", "Failed to publish batch, dropping old
// messages" is logged with the retention age, and dropOld prunes samples older
// than the retention window so a later flush can retry. Must hold mu.
//
// OEM 0x2adde0
func (c *Collector) flush() error {
	if c.timer != nil {
		c.timer.Stop()
		c.timer = nil
	}
	if len(c.messages) == 0 {
		return nil
	}

	batch := Batch{
		Firmware: c.firmware,
		ECU:      c.ecu,
		Timezone: c.timezone,
		User:     c.user,
		Messages: c.messages,
	}

	c.log.Debug("Publishing batch")
	start := time.Now()

	payload, err := batch.MarshalBinary()
	if err != nil {
		return err
	}

	topic := fmt.Sprintf("telemetry/%s", c.bikeID)
	if err := c.pub.Publish(topic, payload); err != nil {
		c.log.Warn("Failed to publish batch, dropping old messages",
			zap.Duration("age", retentionDefault))
		c.messages = dropOld(c.messages, retentionDefault)
		// keep the timer disarmed; a later trigger re-attempts the flush
		c.timer = time.AfterFunc(retentionDefault, func() { _ = c.Flush() })
		return fmt.Errorf("publish: %w", err)
	}

	c.log.Debug("Batch sent", zap.Duration("duration", time.Since(start)))
	c.messages = nil
	return nil
}

// dropOld returns a copy of msgs with every sample older than maxAge removed
// (and topics that end up empty dropped). Cutoff is now-maxAge.
//
// OEM 0x2ae3d0
func dropOld(msgs map[string][]Message, maxAge time.Duration) map[string][]Message {
	cutoff := time.Now().Add(-maxAge)
	out := make(map[string][]Message, len(msgs))
	for topic, list := range msgs {
		kept := make([]Message, 0, len(list))
		for _, m := range list {
			if m.Timestamp.After(cutoff) {
				kept = append(kept, m)
			}
		}
		if len(kept) > 0 {
			out[topic] = kept
		}
	}
	return out
}

// handleMessage is the bus subscription callback. Deny-listed topics are
// dropped ("Ignoring message on topic"); the CBOR "payload" body is decoded and
// a decode failure logs "Ignoring invalid CBOR message". Otherwise the message
// is appended to the per-topic buffer under the lock and, once the buffer
// reaches the configured size, a flush is scheduled ("Scheduling flush").
//
// OEM 0x2ae700
func (c *Collector) handleMessage(msg *paho.Publish) {
	now := time.Now()

	if IgnoreTopic(msg.Topic) {
		c.log.Debug("Ignoring message on topic", zap.String("topic", msg.Topic))
		return
	}

	var attrs Attributes
	if err := cbor.Unmarshal(msg.Payload, &attrs); err != nil {
		c.log.Warn("Ignoring invalid CBOR message",
			zap.String("topic", msg.Topic),
			zap.Binary("payload", msg.Payload),
			zap.Error(err))
		return
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if c.messages == nil {
		c.messages = make(map[string][]Message)
	}
	c.messages[msg.Topic] = append(c.messages[msg.Topic], Message{
		Topic:      msg.Topic,
		Timestamp:  now,
		Attributes: attrs,
		Data:       msg.Payload,
	})

	if c.timer == nil {
		c.log.Debug("Scheduling flush", zap.Duration("delay", c.interval))
		c.timer = time.AfterFunc(c.interval, func() { _ = c.Flush() })
	}
}

// Shutdown unsubscribes from every topic and flushes any remaining buffered
// messages. A flush error is wrapped "flush: %w".
//
// OEM 0x2ac360
func (c *Collector) Shutdown(ctx context.Context) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	// Clearing the subscription set unsubscribes everything.
	_ = c.setSubscriptions(ctx, nil)
	if err := c.flush(); err != nil {
		return fmt.Errorf("flush: %w", err)
	}
	return nil
}

// uuidp formats a 16-byte BLE user UUID as the canonical 36-char string. A nil
// slice yields the empty string.
//
// OEM 0x2adb90
func uuidp(b []byte) string {
	if len(b) == 0 {
		return ""
	}
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

// contains reports whether s holds v.
//
// OEM source: collector.go (telemetry.contains)
func contains(s []string, v string) bool {
	for i := range s {
		if s[i] == v {
			return true
		}
	}
	return false
}

// diff returns the elements of a that are not in b.
//
// OEM source: collector.go (telemetry.diff)
func diff(a, b []string) []string {
	var out []string
	for _, v := range a {
		if !contains(b, v) {
			out = append(out, v)
		}
	}
	return out
}
