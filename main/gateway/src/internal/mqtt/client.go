// client.go implements the loopback MQTT v5 client used by the gateway to talk
// to the on-device broker. It wraps github.com/eclipse/paho.golang/paho, adds
// a TCP dial step, lazy (re)connect on the first publish/subscribe, automatic
// reconnect with a fixed delay on unexpected disconnects, and dispatch of
// inbound PUBLISH messages to handlers keyed by topic Pattern.
package mqtt

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"sync"
	"time"

	"github.com/eclipse/paho.golang/paho"
	"go.uber.org/zap"
)

// reconnectTimeout bounds a single reconnect attempt's connect call.
//
// OEM: literal 15000000000ns passed to context.WithTimeout in attemptReconnect.
const reconnectTimeout = 15 * time.Second

// Config configures a Client.
type Config struct {
	// Endpoint is the broker URL, e.g. "tcp://127.0.0.1:1883". Parsed with
	// net/url; the host:port is used for the TCP dial.
	Endpoint string
	// ClientID is the MQTT client identifier sent in CONNECT.
	ClientID string
	// KeepAlive is the MQTT keepalive interval; sent (in seconds) in CONNECT.
	KeepAlive time.Duration
	// ConnectTimeout bounds dialing + the MQTT CONNECT exchange. Defaults to
	// 60s when zero.
	ConnectTimeout time.Duration
	// ConnectRetryDelay is the fixed back-off between automatic reconnect
	// attempts after an unexpected disconnect. Defaults to
	// defaultReconnectDelay (1s) when zero.
	//
	// OEM: gateway.New stores 1e9 ns ("ConnectRetryDelay 1s") into the loopback
	// Client's reconnectDelay field.
	ConnectRetryDelay time.Duration
	// TLSConfig, when non-nil, switches the dial step to a TLS handshake over
	// the TCP socket (the AWS-IoT mTLS transport). When nil the broker is
	// reached over plain TCP (the loopback transport).
	//
	// OEM: dial branches on this field — non-nil performs the TLS client
	// handshake ("tls handshake: %w"), nil returns the raw TCP conn.
	TLSConfig *tls.Config
}

// defaultReconnectDelay is the fixed auto-reconnect back-off applied when a
// Config does not specify one.
//
// OEM: gateway.New seeds the loopback Client's reconnectDelay with 1e9 ns
// alongside the 1s KeepAlive ("ConnectRetryDelay 1s").
const defaultReconnectDelay = 1 * time.Second

// NewClient constructs a Client from cfg and log. It wires the configuration
// and reconnect back-off; the TCP/TLS connection is established lazily on the
// first Publish/Subscribe (and re-established on demand by the reconnect path).
//
// The loopback transport passes a plain Config{Endpoint, ClientID, KeepAlive};
// the AWS-IoT transport additionally sets Config.TLSConfig to drive the mTLS
// handshake. conn/handlers/reconnect/reconnectTimer all start at their zero
// values.
//
// OEM 0x2b6680 (gateway.New) / 0x2b0c20 (iot.NewClient) — both inline this
// allocation; there is no standalone mqtt.NewClient symbol in the image.
func NewClient(cfg Config, log *zap.Logger) *Client {
	delay := cfg.ConnectRetryDelay
	if delay <= 0 {
		delay = defaultReconnectDelay
	}
	return &Client{
		cfg:            cfg,
		log:            log,
		reconnectDelay: delay,
	}
}

// handler associates a parsed topic Pattern with the callback to run for every
// matching inbound message. The Client keeps these in a slice.
type handler struct {
	pattern  Pattern
	callback func(*paho.Publish)
}

// Connection bundles the live TCP socket with the paho client driving it.
type Connection struct {
	conn   net.Conn     // raw TCP connection (offset 0x18)
	client *paho.Client // paho v5 client (offset 0x90)
}

// Client is a self-(re)connecting MQTT v5 client. The zero value is not usable;
// construct via New. All exported methods are safe for concurrent use.
type Client struct {
	cfg            Config
	log            *zap.Logger
	reconnectDelay time.Duration // offset 0x50

	mu             sync.RWMutex // offset 0x78 — guards conn/handlers/timer
	conn           *Connection  // offset 0x90 — nil while disconnected
	handlers       []handler    // offset 0x98 — topic-routing table
	reconnect      bool         // offset 0xb8 — auto-reconnect armed
	reconnectTimer *time.Timer  // offset 0xc0 — pending scheduled reconnect
}

// connect dials the broker and performs the MQTT CONNECT exchange, wiring the
// message/error/disconnect callbacks. It first cancels any pending reconnect.
// The caller holds c.mu.
//
// OEM 0x260280
func (c *Client) connect(ctx context.Context) error {
	c.abortReconnect()

	c.log.Info("Dialing TCP endpoint", zap.String("endpoint", c.cfg.Endpoint))
	conn, err := dial(ctx, c.cfg.Endpoint, c.cfg.TLSConfig)
	if err != nil {
		return err
	}

	c.log.Info("Connecting to MQTT broker", zap.String("client_id", c.cfg.ClientID))

	pc := paho.NewClient(paho.ClientConfig{
		Conn: conn,
		// Inbound PUBLISH dispatch: every received message is routed through
		// handleMessage, which fans it out to the topic-pattern handlers.
		OnPublishReceived: []func(paho.PublishReceived) (bool, error){
			func(pr paho.PublishReceived) (bool, error) {
				c.handleMessage(pr.Packet)
				return true, nil
			},
		},
		OnClientError:      c.handleClientError,
		OnServerDisconnect: c.handleDisconnectPacket,
		PingHandler:        paho.NewDefaultPinger(),
	})

	cp := &paho.Connect{
		ClientID:   c.cfg.ClientID,
		KeepAlive:  uint16(c.cfg.KeepAlive / time.Second),
		CleanStart: true,
	}

	ca, err := pc.Connect(ctx, cp)
	if err != nil {
		return fmt.Errorf("connect mqtt: %w", err)
	}
	if ca.ReasonCode != 0 {
		return fmt.Errorf("unexpected reason code returned: 0x%02x", ca.ReasonCode)
	}

	c.log.Info("Connected to broker")
	c.conn = &Connection{conn: conn, client: pc}
	return nil
}

// dial parses the broker URL and opens a TCP connection to its host. When
// tlsConfig is non-nil it then performs the TLS client handshake over the TCP
// socket and returns the encrypted connection (the AWS-IoT mTLS transport);
// otherwise the raw TCP connection is returned (the loopback transport).
//
// OEM 0x2612a0
func dial(ctx context.Context, endpoint string, tlsConfig *tls.Config) (net.Conn, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, fmt.Errorf("parse endpoint: %w", err)
	}

	var d net.Dialer
	conn, err := d.DialContext(ctx, "tcp", u.Host)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", u.Host, err)
	}

	if tlsConfig == nil {
		return conn, nil
	}

	tlsConn := tls.Client(conn, tlsConfig)
	if err := tlsConn.HandshakeContext(ctx); err != nil {
		conn.Close()
		return nil, fmt.Errorf("tls handshake: %w", err)
	}
	return tlsConn, nil
}

// Disconnect tears down the connection and disarms auto-reconnect.
//
// OEM 0x260cc0
func (c *Client) Disconnect() {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.abortReconnect()
	c.reconnect = false
	if c.conn != nil {
		c.disconnect()
	}
}

// abortReconnect stops and clears any pending reconnect timer.
// The caller holds c.mu.
//
// OEM 0x260e00
func (c *Client) abortReconnect() {
	if c.reconnectTimer != nil {
		c.reconnectTimer.Stop()
		c.reconnectTimer = nil
	}
}

// disconnect sends an MQTT DISCONNECT, closes the TCP socket and clears c.conn.
// The caller holds c.mu and c.conn is non-nil.
//
// OEM 0x260ea0
func (c *Client) disconnect() {
	c.log.Info("Sending DISCONNECT packet")
	if err := c.conn.client.Disconnect(&paho.Disconnect{ReasonCode: 0}); err != nil {
		c.log.Warn("Could not send DISCONNECT packet", zap.Error(err))
	}

	c.log.Info("Disconnecting TCP connection")
	if err := c.conn.conn.Close(); err != nil {
		c.log.Error("Could not close TCP connection", zap.Error(err))
	}

	c.conn = nil
}

// Publish sends an MQTT PUBLISH, connecting first if necessary.
//
// OEM 0x261f80
func (c *Client) Publish(ctx context.Context, topic string, payload []byte, qos byte, retain bool) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.reconnect = true
	if c.conn == nil {
		if err := c.connect(ctx); err != nil {
			return fmt.Errorf("connect: %w", err)
		}
	}

	_, err := c.conn.client.Publish(ctx, &paho.Publish{
		Topic:   topic,
		Payload: payload,
		QoS:     qos,
		Retain:  retain,
	})
	return err
}

// Subscribe registers handlers for the given topic filters and sends the MQTT
// SUBSCRIBE, connecting first if necessary. Each subscription's topic is parsed
// into a Pattern and added to the dispatch table.
//
// OEM 0x262260
func (c *Client) Subscribe(ctx context.Context, subscriptions []Subscription) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.reconnect = true
	if c.conn == nil {
		if err := c.connect(ctx); err != nil {
			return fmt.Errorf("connect: %w", err)
		}
	}

	subs := make([]paho.SubscribeOptions, 0, len(subscriptions))
	for _, s := range subscriptions {
		pattern := ParsePattern(s.Topic)
		if pattern == nil {
			return fmt.Errorf("parse pattern: %w", errInvalidTopic)
		}
		c.handlers = append(c.handlers, handler{pattern: pattern, callback: s.Callback})
		subs = append(subs, paho.SubscribeOptions{Topic: s.Topic, QoS: s.QoS})
	}

	if _, err := c.conn.client.Subscribe(ctx, &paho.Subscribe{Subscriptions: subs}); err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}
	return nil
}

// Unsubscribe sends the MQTT UNSUBSCRIBE for the given filters and removes the
// matching handlers from the dispatch table. It is a no-op when disconnected.
//
// OEM 0x2626a0
func (c *Client) Unsubscribe(ctx context.Context, topics []string) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn == nil {
		return errNotConnected
	}

	if _, err := c.conn.client.Unsubscribe(ctx, &paho.Unsubscribe{Topics: topics}); err != nil {
		return fmt.Errorf("unsubscribe: %w", err)
	}

	// Drop every handler whose pattern equals one of the unsubscribed filters.
	for _, topic := range topics {
		for i := 0; i < len(c.handlers); i++ {
			if c.handlers[i].pattern.String() == topic {
				c.handlers = append(c.handlers[:i], c.handlers[i+1:]...)
				i--
			}
		}
	}
	return nil
}

// handleMessage is the paho message router callback. It snapshots the handlers
// whose pattern matches the topic (under the read lock), then runs each match.
//
// OEM 0x262b20
func (c *Client) handleMessage(m *paho.Publish) {
	c.mu.RLock()
	var matched []handler
	for _, h := range c.handlers {
		if h.pattern.Match(m.Topic) {
			matched = append(matched, h)
		}
	}
	c.mu.RUnlock()

	for _, h := range matched {
		h.callback(m)
	}
}

// handleClientError is the paho OnClientError callback. It classifies the error
// and then triggers the disconnect/reconnect path.
//
// OEM 0x261620
func (c *Client) handleClientError(err error) {
	switch {
	case errors.Is(err, net.ErrClosed):
		c.log.Warn("Connection closed by server")
	case errors.Is(err, io.EOF):
		c.log.Warn("Connection dropped")
	default:
		c.log.Error("MQTT client error",
			zap.String("type", fmt.Sprintf("%T", err)),
			zap.Error(err))
	}
	c.handleDisconnect()
}

// handleDisconnectPacket is the paho OnServerDisconnect callback for a
// broker-initiated DISCONNECT.
//
// OEM 0x2618e0
func (c *Client) handleDisconnectPacket(d *paho.Disconnect) {
	c.log.Warn("Server sent DISCONNECT packet",
		zap.String("reason_code", fmt.Sprintf("0x%02x", d.ReasonCode)))
	c.handleDisconnect()
}

// handleDisconnect drops the dead connection and, if auto-reconnect is armed,
// schedules a reconnect.
//
// OEM 0x261a30
func (c *Client) handleDisconnect() {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn == nil {
		return
	}
	c.conn = nil
	if c.reconnect && c.reconnectDelay > 0 {
		c.scheduleReconnect()
	}
}

// scheduleReconnect arms a one-shot timer to attempt a reconnect after the
// configured delay. The caller holds c.mu; it is a no-op if a timer is pending.
//
// OEM 0x261ba0
func (c *Client) scheduleReconnect() {
	if c.reconnectTimer != nil {
		return
	}
	c.log.Info("Scheduling reconnect", zap.Duration("wait", c.reconnectDelay))
	c.reconnectTimer = time.AfterFunc(c.reconnectDelay, c.attemptReconnect)
}

// attemptReconnect runs from the reconnect timer: it reconnects under a bounded
// context and, on failure, reschedules another attempt.
//
// OEM 0x261d30
func (c *Client) attemptReconnect() {
	c.mu.Lock()
	defer c.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), reconnectTimeout)
	defer cancel()

	c.reconnectTimer = nil
	if err := c.connect(ctx); err != nil {
		c.log.Warn("Reconnect failed", zap.Error(err))
		c.scheduleReconnect()
	}
}

// debugLogger adapts a *zap.Logger to the paho debug-logging interface,
// forwarding paho's internal trace output at Debug level.
type debugLogger struct {
	log *zap.Logger
}

// Println forwards a paho debug line (formatted with fmt.Sprint).
//
// OEM 0x261170
func (l debugLogger) Println(v ...any) {
	l.log.Debug(fmt.Sprint(v...))
}

// Printf forwards a formatted paho debug line.
//
// OEM 0x261200
func (l debugLogger) Printf(format string, v ...any) {
	l.log.Debug(fmt.Sprintf(format, v...))
}

// Subscription pairs a topic filter with the callback for matching messages.
type Subscription struct {
	Topic    string
	QoS      byte
	Callback func(*paho.Publish)
}

var (
	errInvalidTopic = errors.New("invalid topic")
	errNotConnected = errors.New("not connected")
)
