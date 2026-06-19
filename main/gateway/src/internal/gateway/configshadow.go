package gateway

import (
	"context"
	"fmt"
	"os"

	"go.uber.org/zap"

	"github.com/VanMoof/embedded/gateway/internal/iot/shadow"
	"github.com/VanMoof/embedded/gateway/internal/telemetry"
)

// telemetryConfigField is the desired-state field carrying the telemetry
// configuration JSON (consumed by Run -> Collector.SetConfig).
const telemetryConfigField = "telemetry-config"

// bikeIDField is the desired/reported shadow field carrying the bike id.
const bikeIDField = "bike_id"

// ConfigShadow ties the AWS Device Shadow to the local runtime config: it
// pushes the initial telemetry configuration to the collector and keeps the
// bike id in sync between the shadow's desired/reported state and the local
// value.
type ConfigShadow struct {
	log       *zap.Logger
	shadow    *shadow.Client
	collector *telemetry.Collector

	bikeID    string // local bike id (from /run/media/mmcblk2p6/bike_id)
	telemetry telemetry.Config
}

// NewConfigShadow builds the config shadow. bikeID seeds the local bike id
// (from the provisioning data) so HasBikeID / syncBikeID have the local value
// to reconcile against the shadow's desired/reported state. (Inlined into New
// @0x2b6680.)
func NewConfigShadow(log *zap.Logger, sc *shadow.Client, c *telemetry.Collector, bikeID string) *ConfigShadow {
	return &ConfigShadow{
		log:       log,
		shadow:    sc,
		collector: c,
		bikeID:    bikeID,
	}
}

// HasBikeID reports whether a bike id is known (locally or in the shadow).
func (cs *ConfigShadow) HasBikeID() bool { return cs.bikeID != "" }

// TelemetryConfig returns the telemetry configuration fetched from the shadow.
func (cs *ConfigShadow) TelemetryConfig() telemetry.Config { return cs.telemetry }

// Sync fetches the device shadow, applies the initial telemetry config, then
// reconciles the bike id. OEM 0x2b5bc0.
//
//  1. shadow.State(ctx) reads the current classic shadow document.
//  2. The "telemetry-config" desired field is decoded and stored as the
//     initial telemetry config (consumed later by Run -> Collector.SetConfig).
//  3. syncBikeID reconciles "bike_id" between local and shadow desired/reported
//     -> wrapped as "check bike id: %w" on failure.
func (cs *ConfigShadow) Sync(ctx context.Context) error {
	state, err := cs.shadow.State(ctx)
	if err != nil {
		return err
	}

	// The "telemetry-config" desired field is decoded and stored as the initial
	// telemetry config. The desired section is opaque JSON (json.RawMessage); a
	// shadow.Document accessor reads the field, and the embedded JSON is parsed
	// by telemetry.ParseConfig.
	desired := shadow.NewDocument(state.Desired, nil)
	if f := desired.Field(telemetryConfigField); f != nil {
		cfg, err := telemetry.ParseConfig([]byte(f.Value.Raw))
		if err != nil {
			return fmt.Errorf("parse telemetry config: %w", err)
		}
		cs.telemetry = cfg
	}

	if err := cs.syncBikeID(ctx, state); err != nil {
		return fmt.Errorf("check bike id: %w", err)
	}
	return nil
}

// syncBikeID reconciles the bike id between the local value and the shadow's
// desired/reported state. OEM 0x2b5cd0.
//
// Field key "bike_id"; shadow sections "desired"/"reported". Decision:
//
//   - If desired == reported == local: nothing to do; log "Bike id is in sync".
//   - If the shadow carries a desired bike id that differs from the local one,
//     adopt it locally and report it: log "Updating bike id" (actual -> allow),
//     persist via setBikeID, then publish reported state
//     (shadow.SetField + Report) -> "report bike id: %w".
//   - Otherwise report the local id up to the shadow: log "Reporting bike id".
//
// Field/format strings (verified): "bike_id", "desired", "reported",
// "actual"/"allow", "Bike id is in sync", "Reporting bike id",
// "Updating bike id", "report bike id: %w", "set bike id in report: %w",
// "set bike id %q -> %q: %w".
func (cs *ConfigShadow) syncBikeID(ctx context.Context, state *shadow.State) error {
	// The desired/reported sections are opaque JSON (json.RawMessage); a
	// shadow.Document accessor reads the "bike_id" field out of each.
	desiredDoc := shadow.NewDocument(state.Desired, nil)
	reportedDoc := shadow.NewDocument(state.Reported, nil)

	var desired, reported string
	desiredField := desiredDoc.Field(bikeIDField)
	hasDesired := desiredField != nil
	if hasDesired {
		desired = desiredField.String()
	}
	if reportedField := reportedDoc.Field(bikeIDField); reportedField != nil {
		reported = reportedField.String()
	}

	if hasDesired && desired == reported && desired == cs.bikeID {
		cs.log.Info("Bike id is in sync", zap.String("bike_id", cs.bikeID))
		return nil
	}

	if hasDesired && desired != cs.bikeID {
		cs.log.Info("Updating bike id",
			zap.String("actual", cs.bikeID),
			zap.String("allow", desired))
		if err := setBikeID(cs.bikeID, desired); err != nil {
			return fmt.Errorf("set bike id %q -> %q: %w", cs.bikeID, desired, err)
		}
		cs.bikeID = desired
	}

	cs.log.Info("Reporting bike id",
		zap.String("actual", cs.bikeID),
		zap.String("allow", cs.bikeID))

	doc := shadow.NewDocument(nil, nil)
	if err := doc.SetField(bikeIDField, cs.bikeID); err != nil {
		return fmt.Errorf("set bike id in report: %w", err)
	}
	if err := cs.shadow.Report(ctx, doc.JSON()); err != nil {
		return fmt.Errorf("report bike id: %w", err)
	}
	return nil
}

// bikeIDPath is the persistent eMMC config-partition file that holds the bike
// identifier (the same partition as the IoT provisioning files).
const bikeIDPath = "/run/media/mmcblk2p6/bike_id"

// setBikeID persists a new bike id to the local config partition. OEM 0x2b6450
// (folded in the OEM image). The previous value is passed for logging context
// in the caller's "set bike id %q -> %q" message.
func setBikeID(old, new string) error {
	return os.WriteFile(bikeIDPath, []byte(new), 0o644)
}
