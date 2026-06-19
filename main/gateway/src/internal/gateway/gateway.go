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
	fw        bike.FirmwareVersion
}

// New constructs the whole application. OEM 0x2b6680.
//
// Wiring order (each step's failure is wrapped and returned):
//  1. zap logger named "gateway"; sub-loggers "config", "iot", "bleproxy",
//     "telemetry", "jobclient" are derived with logger.Named(...).
//  2. read provisioning data + firmware version from /run/media/mmcblk2p6
//     (bike.ProvisioningData / bike.FirmwareVersion).
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
func New(ctx context.Context, prov bike.ProvisioningData, fw bike.FirmwareVersion) (*Gateway, error) {
	log := zap.L().Named("gateway")

	g := &Gateway{
		log:  log,
		prov: prov,
		fw:   fw,
	}

	// AWS IoT client: TLS 1.2, Amazon-root CertPool, device mTLS cert/key.
	iotClient, err := iot.NewClient(ctx, log.Named("iot"), prov)
	if err != nil {
		return nil, fmt.Errorf("create IoT client: %w", err)
	}
	g.iot = iotClient

	// Loopback bus client.
	g.mqtt = mqtt.NewClient(log, "mqtt://localhost:1883")

	// Telemetry collector + transport router; seed static metadata.
	g.collector = telemetry.NewCollector(log.Named("telemetry"), g.mqtt)
	g.collector.SetECU(prov.ECU())
	g.collector.SetFirmware(fw)
	g.router = telemetry.NewRouter(log.Named("telemetry"), g.iot, g.bleProxy)

	// Device shadow + config shadow.
	g.shadow = shadow.NewClient(log, g.iot)
	g.config = NewConfigShadow(log.Named("config"), g.shadow, g.collector)

	// BLE proxy transport.
	g.bleProxy = ble.NewProxy(log.Named("bleproxy"), g.mqtt)

	// Jobs client, with the single registered handler "log_upload".
	g.jobs = job.NewClient(log.Named("jobclient"), g.iot)
	g.jobs.Register("log_upload", NewLogJobHandler(log, prov))

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
//     event.HandleBLE(..., g.handleBLEAuth)      -> "handle BLE: %w"
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

	if err := event.HandleTimezone(ctx, g.mqtt, g.collector.SetTimezone); err != nil {
		return fmt.Errorf("handle timezone: %w", err)
	}
	if err := event.HandleBLE(ctx, g.mqtt, g.handleBLEAuth); err != nil {
		return fmt.Errorf("handle BLE: %w", err)
	}
	if err := event.HandleProxyConfig(ctx, g.mqtt, g.handleProxyConfig); err != nil {
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
func (g *Gateway) Shutdown(ctx context.Context) {
	if err := g.collector.Shutdown(ctx); err != nil {
		g.log.Error("Error shutting down telemetry collector", zap.Error(err))
	}
	g.iot.Disconnect()
	g.mqtt.Disconnect()
}

// handleBLEAuth handles BLE connect/disconnect bus events. OEM 0x2b7930.
//
// On a "BLE connected" event it logs "BLE connected" with the identity fields
// carried in the event payload — the verified field-name literals are
// "handle", "cert_id", "cert_expiry", "role", "roots", "user_id", "okta_id" —
// then marks telemetry's BLE link up (Collector.SetBLE true). On a
// "BLE disconnected" event it logs "BLE disconnected" (field "handle") and
// marks the link down (Collector.SetBLE false). Any other payload logs
// "Unknown BLE state".
func (g *Gateway) handleBLEAuth(ev event.BLEEvent) {
	switch ev := ev.(type) {
	case *event.BLEConnected:
		g.log.Info("BLE connected",
			zap.String("handle", ev.Handle),
			zap.String("cert_id", ev.CertID),
			zap.Time("cert_expiry", ev.CertExpiry),
			zap.String("role", ev.Role),
			zap.Strings("roots", ev.Roots),
			zap.String("user_id", ev.UserID),
			zap.String("okta_id", ev.OktaID),
		)
		g.collector.SetBLE(true)
	case *event.BLEDisconnected:
		g.log.Info("BLE disconnected", zap.String("handle", ev.Handle))
		g.collector.SetBLE(false)
	default:
		g.log.Error("Unknown BLE state", zap.Any("state", ev))
	}
}

// handleProxyConfig handles a ble/proxy/config bus event. OEM 0x2b8080.
//
// It maps the proxy-mode byte to a telemetry.TransportMode and applies it via
// Router.SetTransportMode, logging "Setting transport mode" with the resolved
// human-readable name. An out-of-range byte logs "Unknown proxy mode" and is
// ignored.
//
// Byte -> mode mapping (verified):
//
//	1 -> "Prefer BLE proxy"
//	2 -> "Only LTE-M modem"
//	3 -> "Send data to both BLE proxy and LTE-M modem"
//
// The full TransportMode name table (indices 0..4) is:
//
//	0 "Prefer BLE proxy"
//	1 "Prefer LTE-M modem"
//	2 "Only BLE proxy"
//	3 "Only LTE-M modem"
//	4 "Send data to both BLE proxy and LTE-M modem"
func (g *Gateway) handleProxyConfig(mode byte) {
	var tm telemetry.TransportMode
	switch mode {
	case 1:
		tm = telemetry.PreferBLEProxy
	case 2:
		tm = telemetry.OnlyLTEMModem
	case 3:
		tm = telemetry.SendBoth
	default:
		g.log.Error("Unknown proxy mode")
		return
	}

	g.log.Info("Setting transport mode", zap.String("transport", tm.String()))
	g.router.SetTransportMode(tm)
}
