package cli

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"text/tabwriter"

	"github.com/nvmms/sdk_version_manager/internal/sdk"
	"github.com/nvmms/sdk_version_manager/internal/terminal"
)

func list(ctx context.Context, args []string, dir string, m *sdk.Manager, in io.Reader, out, diagnostics io.Writer) (int, error) {
	filter, provider := "all", "node"
	refresh, haveFilter, haveProvider := false, false, false
	plain := false
	for _, arg := range args {
		switch arg {
		case "--plain":
			plain = true
		case "--refresh":
			if refresh {
				return 2, errors.New("--refresh specified twice")
			}
			refresh = true
		case "all", "lts", "installed", "--installed":
			if haveFilter {
				return 2, errors.New("specify only one list filter: all, lts, installed")
			}
			filter = arg
			if filter == "--installed" {
				filter = "installed"
			}
			haveFilter = true
		case "node", "go":
			if haveProvider {
				return 2, errors.New("specify only one Provider")
			}
			provider = arg
			haveProvider = true
		default:
			return 2, fmt.Errorf("unsupported list argument %q; usage: svm list [all|lts|installed] [node|go] [--refresh] [--plain] (supported Providers: node, go)", arg)
		}
	}
	if filter == "installed" && refresh {
		return 2, errors.New("--refresh cannot be combined with the offline installed filter")
	}
	if !m.Supports(provider) {
		return 2, fmt.Errorf("unsupported Provider %s", provider)
	}
	if provider == "go" && filter == "lts" {
		return 2, errors.New("Go does not define an LTS channel; use svm list go")
	}
	var versions []sdk.Version
	if filter == "installed" {
		installed, err := m.Versions(provider)
		if err != nil {
			return 1, err
		}
		for _, v := range installed {
			versions = append(versions, sdk.Version{Number: v, Installed: true})
		}
	} else {
		fmt.Fprintf(diagnostics, "Loading %s versions (cache first; --refresh updates the official index)…\n", provider)
		var err error
		versions, err = m.Catalog(ctx, provider, refresh)
		if err != nil {
			return 1, err
		}
	}
	if filter == "lts" {
		filtered := make([]sdk.Version, 0, len(versions))
		for _, v := range versions {
			if v.LTS != "" {
				filtered = append(filtered, v)
			}
		}
		versions = filtered
	}
	if !plain && terminal.Supported(in, out) {
		return browse(ctx, dir, m, provider, versions, out.(*os.File))
	}
	writer := tabwriter.NewWriter(out, 0, 4, 2, ' ', 0)
	fmt.Fprintln(writer, "INSTALLED\tPROVIDER\tVERSION\tLTS\tRELEASED")
	count, installedCount := 0, 0
	for _, v := range versions {
		mark := "-"
		if v.Installed {
			mark = "✓"
			installedCount++
		}
		fmt.Fprintf(writer, "%s\t%s\t%s\t%s\t%s\n", mark, provider, v.Number, v.LTS, v.Date)
		count++
	}
	if err := writer.Flush(); err != nil {
		return 1, err
	}
	if count == 0 {
		fmt.Fprintf(out, "No matching versions. Install with: svm install %s <version>\n", provider)
	} else {
		fmt.Fprintf(out, "\n%d versions, %d installed (✓).\n", count, installedCount)
	}
	return 0, nil
}
