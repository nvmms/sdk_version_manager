package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"

	"github.com/nvmms/sdk_version_manager/internal/cli"
	"github.com/nvmms/sdk_version_manager/internal/process"
	"github.com/nvmms/sdk_version_manager/internal/sdk"
)

func main() {
	manager, err := sdk.New()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	dir, err := os.Getwd()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	// Proxy commands manage interrupts themselves so the child receives the signal.
	ctx := context.Background()
	if len(os.Args) < 2 {
		os.Exit(cli.Run(ctx, os.Args[1:], dir, manager, os.Stdin, os.Stdout, os.Stderr))
	}
	if _, _, ok := manager.Entry(os.Args[1]); !ok {
		var cancel context.CancelFunc
		ctx, cancel = signal.NotifyContext(ctx, process.Signals()...)
		defer cancel()
	}
	exitCode := cli.Run(ctx, os.Args[1:], dir, manager, os.Stdin, os.Stdout, os.Stderr)
	os.Exit(exitCode)
}
