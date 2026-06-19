// Package iot is the gateway's AWS IoT Core client wrapper. It builds the
// mutual-TLS transport (Amazon-root-pinned server trust + the device's mTLS
// client cert), drives an MQTT v5 client (internal/mqtt over Eclipse Paho),
// and layers the telemetry-publish, request/response (Device Shadow) and
// idle-auto-disconnect behaviour on top.
package iot

import (
	"context"
	"crypto/tls"
	"fmt"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/eclipse/paho.golang/paho"
	"github.com/tidwall/gjson"
	"github.com/tidwall/sjson"
	"go.uber.org/zap"

	"github.com/VanMoof/embedded/gateway/internal/iot/ca"
	"github.com/VanMoof/embedded/gateway/internal/mqtt"
	"github.com/VanMoof/embedded/gateway/internal/telemetry"
)

// telemetryTopicFmt is the AWS IoT rules-engine ingest topic for telemetry
// batches; %s is the device serial / thing name.
const telemetryTopicFmt = "$aws/rules/telemetry/%s"

// idleDisconnectMsg is logged when the idle timer fires.
const idleDisconnectMsg = "Disconnecting idle modem connection"

// Client wraps an MQTT v5 transport with TLS, telemetry publishing, shadow
// request/response correlation and an idle-disconnect timer.
//
// OEM field layout (offsets observed in the binary):
//
//	+0x00  mqtt   mqtt.Client (interface: itab @+0x00, data @+0x08)
//	+0x68  mu     sync.Mutex guarding the idle timer
//	+0xd8  timer  *time.Timer (auto-disconnect; nil when no connection)
//	+0xc8  idle   time.Duration (auto-disconnect timeout)
//	+0xe0  seq    int64 request counter (source of the clientToken)
//	       log    *zap.Logger, serial string
type Client struct {
	mqtt   *mqtt.Client
	serial string
	log    *zap.Logger

	idle time.Duration

	mu    sync.Mutex
	timer *time.Timer

	seq int64
}

// NewClient builds the AWS IoT client. It loads the device's mTLS keypair,
// parses the provisioned endpoint, assembles a tls.Config that pins server
// trust to the two embedded Amazon roots (and enforces TLS 1.2), and hands the
// result to the MQTT transport. This is what gateway.New calls to wire the IoT
// side together.
//
// OEM 0x2b0c20  (iot_build_tls_config)
func NewClient(endpoint string, cert, key []byte, serial string, idle time.Duration, log *zap.Logger) (*Client, error) {
	clientCert, err := tls.X509KeyPair(cert, key)
	if err != nil {
		return nil, fmt.Errorf("parse IoT endpoint: %w", err)
	}

	tlsConfig := &tls.Config{
		RootCAs:      ca.CertPool(), // pinned: Amazon Root CA 1 + CA 3 only
		Certificates: []tls.Certificate{clientCert},
		MinVersion:   tls.VersionTLS12, // 0x0303
	}

	// The cloud transport is the same *mqtt.Client driving a TLS net.Conn: the
	// provisioned endpoint plus the mTLS tls.Config are threaded through
	// mqtt.Config (TLSConfig non-nil switches dial to the TLS handshake). The
	// MQTT ClientID is the device serial / thing name.
	transport := mqtt.NewClient(mqtt.Config{
		Endpoint:  endpoint,
		ClientID:  serial,
		TLSConfig: tlsConfig,
	}, log)

	return &Client{
		mqtt:   transport,
		serial: serial,
		log:    log,
		idle:   idle,
	}, nil
}

// Publish marshals a telemetry batch and publishes it to the AWS IoT rules
// topic for this device, then (re)arms the idle auto-disconnect timer so an
// otherwise-quiet LTE-M link is torn down after the configured timeout.
//
// OEM 0x2b10a0
func (c *Client) Publish(batch telemetry.Batch) error {
	payload, err := batch.MarshalBinary()
	if err != nil {
		return fmt.Errorf("marshal batch: %w", err)
	}
	return c.publish(payload)
}

// PublishRaw publishes an already-encoded telemetry payload over the modem link
// (the topic argument is the Router's "telemetry/<bike-id>" key; the AWS rules
// topic is derived from the device serial, as in Publish). It satisfies the
// modem side of telemetry.Transport once adapted by the gateway, so the Router
// can fan an already-flushed batch out without re-marshalling.
func (c *Client) PublishRaw(_ string, payload []byte) error {
	return c.publish(payload)
}

// publish ships an encoded telemetry payload to the AWS IoT rules topic for this
// device and (re)arms the idle auto-disconnect timer. Caller must not hold c.mu.
func (c *Client) publish(payload []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	topic := fmt.Sprintf(telemetryTopicFmt, c.serial)

	c.log.Info("Publishing telemetry")
	if err := c.mqtt.Publish(context.Background(), topic, payload, 1, false); err != nil {
		return fmt.Errorf("publish telemetry: %w", err)
	}

	c.armIdleTimer()
	return nil
}

// armIdleTimer (re)schedules autoDisconnect after c.idle. Caller holds c.mu.
func (c *Client) armIdleTimer() {
	if c.idle <= 0 {
		return
	}
	if c.timer != nil {
		c.timer.Reset(c.idle)
		return
	}
	c.timer = time.AfterFunc(c.idle, c.autoDisconnect)
}

// autoDisconnect is the idle-timer callback: it logs the idle timeout and tears
// the connection down via Disconnect.
//
// OEM 0x2b1810
func (c *Client) autoDisconnect() {
	c.log.Info(idleDisconnectMsg, zap.Duration("ttl", c.idle))
	c.Disconnect()
}

// Disconnect stops the idle timer (if any) and disconnects the MQTT transport.
//
// OEM 0x2b1640
func (c *Client) Disconnect() {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.timer != nil {
		c.timer.Stop()
		c.timer = nil
	}
	c.mqtt.Disconnect()
}

// Perform runs a request/response exchange against an AWS-IoT-style endpoint
// that answers on "<topic>/accepted" and "<topic>/rejected". It mints a unique
// clientToken (a base-36 sequence counter), injects it into the request,
// subscribes to both reply topics, publishes the request, and waits for the
// reply whose clientToken matches — delivering the raw reply PUBLISH (its Topic
// distinguishes accepted vs rejected, its Payload carries the body) back to the
// caller. This is the transport beneath the Device Shadow get/update and the
// Jobs request/response flows.
//
// OEM 0x2b1910
func (c *Client) Perform(ctx context.Context, topic string, payload []byte) (*paho.Publish, error) {
	token := c.nextClientToken()

	accepted := topic + "/accepted"
	rejected := topic + "/rejected"

	resp := make(chan *paho.Publish, 1)

	// OEM 0x2b2060  (Perform.func1) — the reply correlator. Both the
	// "/accepted" and "/rejected" reply topics share this callback; the caller
	// distinguishes them by the reply's Topic suffix.
	handler := func(p *paho.Publish) {
		if matchClientToken(p) != token {
			return // not ours
		}
		select {
		case resp <- p:
		default:
		}
	}

	subs := []mqtt.Subscription{
		{Topic: accepted, QoS: 1, Callback: handler},
		{Topic: rejected, QoS: 1, Callback: handler},
	}
	if err := c.mqtt.Subscribe(ctx, subs); err != nil {
		return nil, fmt.Errorf("subscribe: %w", err)
	}
	defer c.mqtt.Unsubscribe(ctx, []string{accepted, rejected})

	body, err := injectClientToken(payload, token)
	if err != nil {
		return nil, fmt.Errorf("marshal: %w", err)
	}
	if err := c.mqtt.Publish(ctx, topic, body, 2, false); err != nil {
		return nil, fmt.Errorf("publish get: %w", err)
	}

	select {
	case r := <-resp:
		return r, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// Subscribe registers cb for inbound PUBLISHes on topic (QoS 1) and forwards to
// the underlying MQTT transport. The jobs client uses this for notify-next.
//
// OEM: inlined into job.Client.Start (the c.mqtt.Subscribe call).
func (c *Client) Subscribe(ctx context.Context, topic string, cb func(*paho.Publish)) error {
	return c.mqtt.Subscribe(ctx, []mqtt.Subscription{{Topic: topic, QoS: 1, Callback: cb}})
}

// Unsubscribe removes the subscription for topic on the underlying transport.
//
// OEM: inlined into job.Client.Stop (the c.mqtt.Unsubscribe call).
func (c *Client) Unsubscribe(ctx context.Context, topic string) error {
	return c.mqtt.Unsubscribe(ctx, []string{topic})
}

// matchClientToken extracts the clientToken from an inbound shadow/job reply so
// the waiting Perform call can correlate it. AWS IoT replies are JSON; the
// clientToken lives at the document root.
func matchClientToken(p *paho.Publish) string {
	return gjson.GetBytes(p.Payload, "clientToken").String()
}

// injectClientToken sets the "clientToken" field on a JSON request document so
// the matching reply can be correlated. sjson returns a fresh buffer.
func injectClientToken(request []byte, token string) ([]byte, error) {
	return sjson.SetBytes(request, "clientToken", token)
}

// nextClientToken returns the next request token: an atomically-incremented
// counter rendered in base 36 (OEM: strconv.FormatInt(seq, 36)).
func (c *Client) nextClientToken() string {
	n := atomic.AddInt64(&c.seq, 1)
	return strconv.FormatInt(n, 36)
}
