// Package gateway is the application spine of the VanMoof bike <-> AWS IoT
// cloud bridge service (github.com/VanMoof/embedded/gateway/internal/gateway).
//
// It wires together the local loopback MQTT bus, the AWS IoT client (mutual
// TLS, Amazon-root CertPool), the device shadow + jobs channels, and the
// telemetry collector/router, then runs the main loop that subscribes the
// local-bus event handlers and pumps cloud config/state/telemetry.
//
// Only VanMoof application code is reconstructed here; the vendor packages
// (go.uber.org/zap, the Paho MQTT v5 client, fxamacker/cbor, crypto/tls,
// net/http, os/exec, …) are imported and modelled.
package gateway

import (
	"context"
	"fmt"
	"time"

	"github.com/eclipse/paho.golang/paho"
	"go.uber.org/zap"

	"github.com/VanMoof/embedded/gateway/internal/bike"
	"github.com/VanMoof/embedded/gateway/internal/ble"
	"github.com/VanMoof/embedded/gateway/internal/event"
	"github.com/VanMoof/embedded/gateway/internal/iot"
	"github.com/VanMoof/embedded/gateway/internal/iot/job"
	"github.com/VanMoof/embedded/gateway/internal/iot/shadow"
	"github.com/VanMoof/embedded/gateway/internal/mqtt"
	"github.com/VanMoof/embedded/gateway/internal/telemetry"
)

// eventBus adapts the loopback *mqtt.Client to the event.Bus seam. event.Bus
// wants a single-topic Subscribe whose callback receives (topic, payload);
// mqtt.Client.Subscribe takes (ctx, []mqtt.Subscription) with a
// func(*paho.Publish) callback. This shim bridges the two settled seams (the
// OEM event handlers subscribe through the same bus the collector uses).
type eventBus struct {
	ctx    context.Context
	client *mqtt.Client
}

// Subscribe satisfies event.Bus, forwarding to the mqtt client's
// (ctx, []mqtt.Subscription) Subscribe and translating each inbound
// *paho.Publish into the (topic, payload) callback shape.
func (b eventBus) Subscribe(topic string, handler func(topic string, payload []byte)) error {
	return b.client.Subscribe(b.ctx, []mqtt.Subscription{{
		Topic: topic,
		QoS:   1,
		Callback: func(p *paho.Publish) {
			handler(p.Topic, p.Payload)
		},
	}})
}

// busPublisher adapts the loopback *mqtt.Client to ble.Publisher. ble.Publisher
// wants Publish(topic, payload, qos, retain) with NO context; mqtt.Client.Publish
// takes a leading context.Context, so this shim binds a background context in
// front of the bus publish (the BLE proxy republishes onto the local bus).
type busPublisher struct {
	client *mqtt.Client
}

// Publish satisfies ble.Publisher, prefixing the bound context to the bus
// publish.
func (p busPublisher) Publish(topic string, payload []byte, qos byte, retain bool) error {
	return p.client.Publish(context.Background(), topic, payload, qos, retain)
}

// modemTransport adapts the AWS *iot.Client to telemetry.Transport. The Router
// hands an already-flushed (CBOR+zlib-encoded) batch as (topic, payload); the
// modem link forwards it via PublishRaw (the AWS rules topic is derived from the
// device serial inside the iot client).
type modemTransport struct {
	client *iot.Client
}

// Publish satisfies telemetry.Transport for the modem link.
func (t modemTransport) Publish(topic string, payload []byte) error {
	return t.client.PublishRaw(topic, payload)
}

// rawBatch wraps an already-encoded telemetry payload so it satisfies ble.Batch
// (MarshalBinary returns the bytes verbatim); the BLE proxy CBOR-wraps and
// republishes it onto the relay topic.
type rawBatch []byte

// MarshalBinary returns the pre-encoded batch bytes unchanged.
func (b rawBatch) MarshalBinary() ([]byte, error) { return b, nil }

// proxyTransport adapts the *ble.Proxy to telemetry.Transport. The Router hands
// an already-flushed batch as (topic, payload); the proxy republishes it onto
// the BLE relay topic when the BLE side is connected.
type proxyTransport struct {
	proxy *ble.Proxy
}

// Publish satisfies telemetry.Transport for the BLE proxy link.
func (t proxyTransport) Publish(topic string, payload []byte) error {
	return t.proxy.Publish(rawBatch(payload))
}

// Gateway is the fully-wired application. It owns the MQTT bus client, the AWS
// IoT client, the telemetry collector and transport router, the jobs client,
// the config shadow, and the named loggers. Run drives it; Shutdown tears it
// down in reverse order.
type Gateway struct {
	log       *zap.Logger
	mqtt      *mqtt.Client // loopback bus client (mqtt://localhost:1883)
	iot       *iot.Client  // AWS IoT Core client (mTLS)
	collector *telemetry.Collector
	router    *telemetry.Router
	bleProxy  *ble.Proxy     // BLE transport for telemetry when no modem
	jobs      *job.Client    // AWS IoT Jobs (cloud -> bike commands)
	shadow    *shadow.Client // AWS Device Shadow client
	config    *ConfigShadow  // config-shadow sync (telemetry config + bike id)
	prov      bike.ProvisioningData
	fw        string // firmware version (etc/firmware_version)

	telemetryConfig telemetry.Config // initial telemetry config (from run.go)
}

// New constructs the whole application. OEM 0x2b6680.
//
// Wiring order (each step's failure is wrapped and returned):
//  1. zap logger named "gateway"; sub-loggers "config", "iot", "bleproxy",
//     "telemetry", "jobclient" are derived with logger.Named(...).
//  2. provisioning data + firmware version are loaded by the caller (run.go)
//     from /run/media/mmcblk2p6 (bike.LoadProvisioningData / bike.FirmwareVersion)
//     and handed in.
//  3. iot.NewClient(...) builds the tls.Config (TLS 1.2 min, Amazon-root-only
//     CertPool, device cert/key) and the AWS IoT connection -> on error
//     "create IoT client: %w".
//  4. mqtt.Client connected to the loopback broker "mqtt://localhost:1883"
//     (KeepAlive 1s; ConnectRetryDelay 1s).
//  5. telemetry.Collector + telemetry.Router (the Router decides modem vs BLE
//     proxy transport); Collector.SetECU / SetFirmware seed static fields.
//  6. shadow.Client (classic Device Shadow) + ConfigShadow built on top.
//  7. job.Client; the single job handler "log_upload" is registered into its
//     handler map (mapassign, key "log_upload") -> NewLogJobHandler.
//
// Returns the assembled *Gateway.
func New(log *zap.Logger, prov bike.ProvisioningData, fw string, cfg telemetry.Config, autoDisconnect time.Duration) (*Gateway, error) {
	log = log.Named("gateway")

	g := &Gateway{
		log:             log,
		prov:            prov,
		fw:              fw,
		telemetryConfig: cfg,
	}

	// AWS IoT client: TLS 1.2, Amazon-root CertPool, device mTLS cert/key.
	iotClient, err := iot.NewClient(prov.Endpoint, prov.Certificate, prov.PrivateKey, prov.Serial, autoDisconnect, log.Named("iot"))
	if err != nil {
		return nil, fmt.Errorf("create IoT client: %w", err)
	}
	g.iot = iotClient

	// Loopback bus client (mqtt://localhost:1883). KeepAlive 1s; the auto-reconnect
	// back-off defaults to 1s ("ConnectRetryDelay 1s").
	g.mqtt = mqtt.NewClient(mqtt.Config{
		Endpoint:  "mqtt://localhost:1883",
		ClientID:  "gateway",
		KeepAlive: time.Second,
	}, log.Named("mqtt"))

	// BLE proxy transport. NewProxy is (ble.Publisher, *zap.Logger); the loopback
	// bus client is the publisher, adapted to drop the leading context.
	g.bleProxy = ble.NewProxy(busPublisher{g.mqtt}, log.Named("bleproxy"))

	// Transport router: modem link is the AWS IoT client, proxy link is the BLE
	// proxy. Each is adapted to telemetry.Transport's (topic, payload) Publish.
	// Built before the collector, since the router is the collector's publisher.
	g.router = telemetry.NewRouter(modemTransport{g.iot}, proxyTransport{g.bleProxy})

	// Telemetry collector + static metadata. The collector subscribes the local
	// bus and flushes batches through the router; bikeID keys the cloud topic.
	g.collector = telemetry.NewCollector(log.Named("telemetry"), g.mqtt, g.router, prov.BikeID)
	g.collector.SetFirmware(fw)
	g.collector.SetECU(prov.Serial)

	// Device shadow + config shadow. The shadow rides the correlated IoT client
	// (Perform); "configshadow" is the named shadow.
	g.shadow = shadow.NewClient(g.iot, prov.Serial, "configshadow")
	g.config = NewConfigShadow(log.Named("config"), g.shadow, g.collector, prov.BikeID)

	// Jobs client, with the single registered handler "log_upload".
	g.jobs = job.NewClient(g.iot, prov.Serial, log.Named("jobclient").Sugar())
	g.jobs.Register("log_upload", NewLogJobHandler(log, prov).Handle)

	return g, nil
}

// Run is the main loop. OEM 0x2b7070.
//
// Steps:
//  1. config.Sync() pushes the initial telemetry config and syncs the bike id
//     -> on error "get remote config: %w".
//  2. If the bike has no id locally or in the shadow, log a warning
//     ("No bike id set locally or in shadow, shutting down") and return.
//  3. Log "Start gateway".
//  4. Subscribe the local-bus event handlers:
//     event.HandleTimezone(...)     -> "handle timezone: %w"
//     event.HandleBLE(..., g)       -> "handle BLE: %w"
//     event.HandleProxyConfig(..., g.handleProxyConfig) -> "handle proxy config: %w"
//  5. jobs.Start(ctx)              -> "starting job client: %w"
//  6. collector.SetConfig(...)     -> "set initial telemetry config: %w"
func (g *Gateway) Run(ctx context.Context) error {
	if err := g.config.Sync(ctx); err != nil {
		return fmt.Errorf("get remote config: %w", err)
	}

	if !g.config.HasBikeID() {
		g.log.Warn("No bike id set locally or in shadow, shutting down")
		return nil
	}

	g.log.Info("Start gateway")

	bus := eventBus{ctx: ctx, client: g.mqtt}
	if err := event.HandleTimezone(bus, g.setTimezone); err != nil {
		return fmt.Errorf("handle timezone: %w", err)
	}
	if err := event.HandleBLE(bus, g); err != nil {
		return fmt.Errorf("handle BLE: %w", err)
	}
	if err := event.HandleProxyConfig(bus, g.handleProxyConfig); err != nil {
		return fmt.Errorf("handle proxy config: %w", err)
	}

	if err := g.jobs.Start(ctx); err != nil {
		return fmt.Errorf("starting job client: %w", err)
	}

	if err := g.collector.SetConfig(ctx, g.config.TelemetryConfig()); err != nil {
		return fmt.Errorf("set initial telemetry config: %w", err)
	}

	return nil
}

// Shutdown tears the gateway down. OEM 0x2b7780.
//
// It shuts the telemetry collector first (logging "Error shutting down
// telemetry collector" on failure), then disconnects the AWS IoT client and
// finally the loopback MQTT client. The deferred cancel/cleanup runs last.
//
// grace bounds the collector's final flush/shutdown.
func (g *Gateway) Shutdown(grace time.Duration) {
	ctx, cancel := context.WithTimeout(context.Background(), grace)
	defer cancel()

	if err := g.collector.Shutdown(ctx); err != nil {
		g.log.Error("Error shutting down telemetry collector", zap.Error(err))
	}
	g.iot.Disconnect()
	g.mqtt.Disconnect()
}

// setTimezone adapts the modem timezone offset (minutes east of UTC, from
// event.HandleTimezone) to the collector's string-offset SetTimezone. The
// offset is rendered as a signed "±HH:MM" wall-clock offset.
//
// OEM (folded into the timezone handler closure @0x2b7070).
func (g *Gateway) setTimezone(minutes int) {
	sign := "+"
	if minutes < 0 {
		sign = "-"
		minutes = -minutes
	}
	g.collector.SetTimezone(fmt.Sprintf("%s%02d:%02d", sign, minutes/60, minutes%60))
}

// SetBLEAuthenticated handles a "BLE connected/authenticated" bus event
// (event.BLEAuth). OEM 0x2b7930.
//
// It logs "BLE connected" with the connection handle and the time the phone
// passed the VanMoof BLE challenge, then marks telemetry's BLE link up. The
// 16-byte user UUID carried by the collector keys batch attribution; here the
// handle is supplied to Collector.SetBLE.
func (g *Gateway) SetBLEAuthenticated(handle string, at time.Time) {
	g.log.Info("BLE connected",
		zap.String("handle", handle),
		zap.Time("authenticated_at", at),
	)
	g.collector.SetBLE([]byte(handle))
}

// SetBLEInvalid handles a "BLE disconnected/invalid" bus event
// (event.BLEAuth). OEM 0x2b7930.
//
// It logs "BLE disconnected" (field "handle") and clears telemetry's BLE
// attribution (an empty UUID marks the link down).
func (g *Gateway) SetBLEInvalid(handle string) {
	g.log.Info("BLE disconnected", zap.String("handle", handle))
	g.collector.SetBLE(nil)
}

// handleProxyConfig handles a ble/proxy/config bus event. OEM 0x2b8080.
//
// It maps the event transport mode to a telemetry.TransportMode and applies it
// via Router.SetTransportMode, logging "Setting transport mode" with the
// resolved human-readable name. An out-of-range mode logs "Unknown proxy mode"
// and is ignored.
//
// event.TransportMode -> telemetry.TransportMode mapping (verified):
//
//	1 (ble)   -> OnlyProxy   "Only BLE proxy"
//	2 (modem) -> OnlyModem   "Only LTE-M modem"
//	3 (both)  -> Both        "Send data to both BLE proxy and LTE-M modem"
func (g *Gateway) handleProxyConfig(mode event.TransportMode) {
	var tm telemetry.TransportMode
	switch mode {
	case 1: // ble
		tm = telemetry.OnlyProxy
	case 2: // modem
		tm = telemetry.OnlyModem
	case 3: // both
		tm = telemetry.Both
	default:
		g.log.Error("Unknown proxy mode")
		return
	}

	g.log.Info("Setting transport mode", zap.String("transport", tm.String()))
	g.router.SetTransportMode(tm)
}
