package main

import (
	"context"
	"flag"
	"fmt"
	"io/fs"
	"os"
	"time"

	"github.com/peterbourgon/ff/v3"
	"github.com/peterbourgon/ff/v3/ffcli"

	"github.com/VanMoof/embedded/gateway/internal/bike"
	gw "github.com/VanMoof/embedded/gateway/internal/gateway"
	"github.com/VanMoof/embedded/gateway/internal/telemetry"
)

// rootDirEnv lets the install relocate the device's read-only roots (the
// mmcblk2p6 provisioning files); empty = the real "/". (OEM env "ROOT_DIR".)
const rootDirEnv = "ROOT_DIR"

// defaultTelemetryConfig is the embedded fallback used when no -telemetry-config
// file is given: collect for 60 s, with the built-in subscription set. (OEM
// rodata `{"max_age_seconds":60,"subscriptions":…}` @ 0x685f50 — only the head is
// recoverable from the symbol; the subscription list is filled by ParseConfig.)
const defaultTelemetryConfig = `{"max_age_seconds":60,"subscriptions":[]}`

// runCommand builds the `run` subcommand: a flag set (verbose, telemetry-config,
// auto-disconnect) wired through peterbourgon/ff (env + optional config file),
// whose Exec starts the bridge.
//
// OEM 0x2befe0
func runCommand() *ffcli.Command {
	fs := flag.NewFlagSet("gateway", flag.ExitOnError)

	verbose := fs.Bool("verbose", false, "Enable verbose logging")
	telemetryConfig := fs.String("telemetry-config", "", "Path to telemetry configuration file")
	autoDisconnect := fs.Duration("auto-disconnect", 0, "Automatically disconnect idle modem after this time")

	return &ffcli.Command{
		Name:       "run",
		ShortUsage: "gateway run [flags]",
		ShortHelp:  "Start gateway",
		FlagSet:    fs,
		Options:    []ff.Option{ff.WithEnvVars()},
		// OEM 0x2bf330 (runCommand.func1)
		Exec: func(ctx context.Context, _ []string) error {
			return runGateway(ctx, *verbose, *telemetryConfig, *autoDisconnect)
		},
	}
}

// runGateway is the `run` Exec body: build the logger, read the firmware
// version + device provisioning off mmcblk2p6, load (or default) the telemetry
// config, construct the gateway and run it until the context is cancelled, then
// shut down with a 10 s grace period.
//
// OEM 0x2bf330 (runCommand.func1)
func runGateway(ctx context.Context, verbose bool, telemetryConfigPath string, autoDisconnect time.Duration) error {
	log := gw.NewLogger(verbose) // OEM 0x2b92a0

	// ROOT_DIR relocates the read-only device roots; empty = the real "/".
	// os.DirFS rejects "", so an unset ROOT_DIR maps to "/". The result also
	// implements fs.ReadFileFS, which FirmwareVersion needs.
	rootDir := os.Getenv(rootDirEnv)
	if rootDir == "" {
		rootDir = "/"
	}
	root := os.DirFS(rootDir).(fs.ReadFileFS)

	fw, err := bike.FirmwareVersion(root) // OEM 0x1c69b0
	if err != nil {
		return fmt.Errorf("read firmware version: %w", err)
	}

	log.Info("Starting gateway")

	prov, err := bike.LoadProvisioningData(root) // OEM 0x1c6bb0 (serial, bike_id, config.cfg, cert, key)
	if err != nil {
		return fmt.Errorf("load provisioning: %w", err)
	}

	// The OEM then iterates the provisioned ECUs to collect their firmware
	// versions (fed to telemetry via Collector.SetECU/SetFirmware inside New).

	var cfg telemetry.Config
	if telemetryConfigPath == "" {
		cfg, err = telemetry.ParseConfig([]byte(defaultTelemetryConfig)) // OEM 0x2af270
	} else {
		cfg, err = telemetry.ReadConfigFile(telemetryConfigPath) // OEM 0x2af170
	}
	if err != nil {
		return fmt.Errorf("load telemetry config: %w", err)
	}

	g, err := gw.New(log, prov, fw, cfg, autoDisconnect) // OEM 0x2b6680
	if err != nil {
		return fmt.Errorf("create gateway: %w", err)
	}

	if err := g.Run(ctx); err != nil { // OEM 0x2b7070
		return fmt.Errorf("run gateway: %w", err)
	}

	log.Info("Shutting down")
	g.Shutdown(10 * time.Second) // OEM 0x2b7780
	return nil
}
