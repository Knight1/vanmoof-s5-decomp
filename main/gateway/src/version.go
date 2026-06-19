package main

import (
	"context"
	"flag"
	"fmt"

	"github.com/peterbourgon/ff/v3/ffcli"
)

// version is the build version, stamped in at link time (-ldflags "-X main.version=…").
var version = "dev"

// versionCommand builds the `version` subcommand: it just prints the build
// version and exits.
//
// OEM 0x2c0090
func versionCommand() *ffcli.Command {
	return &ffcli.Command{
		Name:      "version",
		ShortHelp: "Print version information",
		FlagSet:   flag.NewFlagSet("version", flag.ExitOnError),
		// OEM 0x2c0010 (versionCommand.func1)
		Exec: func(context.Context, []string) error {
			fmt.Println(version)
			return nil
		},
	}
}
