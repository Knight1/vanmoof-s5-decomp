package telemetry

import (
	"encoding/json"
	"fmt"
	"os"
)

// Config is the telemetry configuration delivered via the device shadow
// "telemetry-config" key (and overridable from a local JSON file). It lists the
// bus topics to subscribe to and the maximum age a buffered sample may reach
// before dropOld discards it.
//
// OEM source: config.go (telemetry.Config)
type Config struct {
	// Subscriptions are the local-bus topic patterns the Collector forwards.
	Subscriptions []Subscription `json:"subscriptions"`
	// MaxAge is the retention window for buffered samples, in seconds on the
	// wire. dropOld discards anything older than this once a publish fails.
	MaxAge int `json:"max_age_seconds"`
}

// Subscription is one entry of the telemetry config: a bus topic pattern and
// the cloud Source it is attributed to.
//
// OEM source: config.go (telemetry.Subscription)
type Subscription struct {
	Topic  string `json:"topic"`
	Source Source `json:"source,omitempty"`
}

// Source tags where a measurement came from when it is attributed in the batch.
//
// OEM source: config.go (telemetry.Source)
type Source string

// configFile is the on-disk fallback path for the telemetry config.
//
// OEM source: config.go (telemetry.configFile)
const configFile = "/etc/gateway/telemetry.json"

// ReadConfigFile reads path and parses it as a telemetry Config. A read failure
// is wrapped "open %s: %w"; the bytes are then handed to ParseConfig.
//
// OEM 0x2af170
func ReadConfigFile(path string) (Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Config{}, fmt.Errorf("open %s: %w", path, err)
	}
	return ParseConfig(data)
}

// ParseConfig unmarshals raw telemetry config JSON. A decode failure is wrapped
// "decode config: %w".
//
// OEM 0x2af270
func ParseConfig(raw []byte) (Config, error) {
	var cfg Config
	if err := json.Unmarshal(raw, &cfg); err != nil {
		return Config{}, fmt.Errorf("decode config: %w", err)
	}
	return cfg, nil
}
