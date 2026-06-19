// Command gateway is the VanMoof bike ↔ AWS IoT cloud bridge (the `gateway`
// service). It is a small peterbourgon/ff(cli) CLI with two subcommands —
// `run` (start the bridge) and `version` (print build info). The real work
// lives in the internal/* packages; this file is just the process entry point
// and exit-code mapping.
package main

import (
	"context"
	"errors"
	"flag"
	"os"
	"os/signal"
	"syscall"

	"github.com/peterbourgon/ff/v3/ffcli"
)

// main builds the root command (subcommands run + version), runs it against an
// interrupt-cancellable context, and maps the outcome onto a process exit code.
//
// OEM 0x2bed80
func main() {
	root := &ffcli.Command{
		Name:        "gateway",
		ShortUsage:  "gateway <subcommand> [flags]",
		FlagSet:     flag.NewFlagSet("gateway", flag.ExitOnError),
		Subcommands: []*ffcli.Command{runCommand(), versionCommand()},
		// No subcommand given → show help. (OEM 0x2bef40, main.main.func1)
		Exec: func(context.Context, []string) error { return flag.ErrHelp },
	}

	// SIGINT/SIGTERM cancel the context so `run` shuts the bridge down cleanly.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := root.ParseAndRun(ctx, os.Args[1:]); err != nil {
		// A help request (or a clean context cancellation) is not a real
		// failure: exit 2 for the help/usage path, 1 for anything else.
		if errors.Is(err, flag.ErrHelp) {
			os.Exit(2)
		}
		os.Exit(1)
	}
}
