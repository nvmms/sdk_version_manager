// Package cli handles arguments and presentation; SDK behavior lives in sdk.
package cli

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"github.com/nvmms/sdk_version_manager/internal/terminal"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/sdk"
)

const help = `SVM — SDK Version Manager (Go CLI)
Usage:
  svm use [provider [version]]
  svm install <node|go|flutter> <version>
  svm list [all|lts|stable|beta|installed] [node|go|flutter] [--refresh] [--plain]
  svm default <node|go|flutter> [version|--clear] [--shell bash|zsh|powershell | --no-shell]
  svm uninstall <node|go|flutter> <version> --yes
  svm proxy [show|set <http-url>|clear]
  svm doctor
  svm <node|npm|npx|corepack|go|gofmt|flutter|dart> [arguments...]

use queries or sets the current project version. Selection order:
explicit version, nearest .svmrc, global default, newest installed version.
Missing versions are installed automatically. No noninteractive prompts.
SVM_HOME overrides the local state directory. This migration currently supports Node.js, Go and Flutter.
`

func Run(ctx context.Context, args []string, dir string, m *sdk.Manager, in io.Reader, out, diagnostics io.Writer) int {
	code, err := run(ctx, args, dir, m, in, out, diagnostics)
	if err != nil {
		fmt.Fprintln(diagnostics, "svm:", err)
		if code == 0 {
			return 1
		}
	}
	return code
}
func run(ctx context.Context, args []string, dir string, m *sdk.Manager, in io.Reader, out, diagnostics io.Writer) (int, error) {
	if len(args) == 0 || args[0] == "--help" || args[0] == "-h" {
		fmt.Fprint(out, help)
		return 0, nil
	}
	command, rest := args[0], args[1:]
	// Internal shell protocol. Keep the old env spelling hidden for installed hooks.
	if command == "__env" || command == "env" {
		return manage(ctx, "env", rest, dir, m, out, diagnostics)
	}
	switch command {
	case "init", "shell", "ide", "restore", "repair":
		return 2, fmt.Errorf("command %q is temporarily disabled; svm use initializes projects and svm default configures shell integration automatically", command)
	}
	if _, _, ok := m.Entry(command); ok {
		return m.Run(ctx, dir, command, rest, in, out, diagnostics)
	}
	if len(rest) == 1 && (rest[0] == "--help" || rest[0] == "-h") {
		fmt.Fprint(out, help)
		return 0, nil
	}
	switch command {
	case "install":
		if len(rest) != 2 || !m.Supports(rest[0]) {
			return 2, errors.New("usage: svm install <node|go|flutter> <version>; supported Providers: node, go, flutter")
		}
		return result(m.Install(ctx, rest[0], rest[1], diagnostics))
	case "list":
		return list(ctx, rest, dir, m, in, out, diagnostics)
	case "use":
		if len(rest) >= 3 && rest[2] == "--global" {
			forwarded := append(append([]string{}, rest[:2]...), rest[3:]...)
			return manage(ctx, "default", forwarded, dir, m, out, diagnostics)
		}
		if len(rest) > 2 || len(rest) > 0 && !m.Supports(rest[0]) {
			return 2, errors.New("usage: svm use [provider [version]]; supported Providers: node, go, flutter")
		}
		if len(rest) == 0 {
			path, err := config.Find(dir)
			if err == nil {
				p, err := config.Read(path)
				if err != nil {
					return 1, err
				}
				fmt.Fprintln(out, "Project:", path)
				keys := make([]string, 0, len(p.SDKs))
				for k := range p.SDKs {
					keys = append(keys, k)
				}
				sort.Strings(keys)
				for _, k := range keys {
					fmt.Fprintln(out, k, p.SDKs[k])
					if m.Supports(k) && m.Installed(k, p.SDKs[k]) {
						if err := m.ProjectLink(filepath.Dir(path), k, p.SDKs[k]); err != nil {
							return 1, err
						}
					}
				}
			} else if !errors.Is(err, os.ErrNotExist) {
				return 1, err
			} else {
				fmt.Fprintln(out, "No project declaration.")
			}
			defaults, err := m.Defaults()
			if err != nil {
				return 1, err
			}
			keys := make([]string, 0, len(defaults))
			for k := range defaults {
				keys = append(keys, k)
			}
			sort.Strings(keys)
			for _, k := range keys {
				fmt.Fprintln(out, "Global:", k, defaults[k])
			}
			return 0, nil
		}
		provider := rest[0]
		var version string
		if len(rest) == 2 {
			version = rest[1]
		} else {
			var err error
			version, _, err = m.Resolve(dir, provider, true)
			if errors.Is(err, sdk.ErrNoVersion) && terminal.Supported(in, out) {
				fmt.Fprintf(diagnostics, "Enter an exact %s version: ", provider)
				scanner := bufio.NewScanner(in)
				if !scanner.Scan() {
					return 3, fmt.Errorf("no version entered; run svm use %s <version>", provider)
				}
				version = strings.TrimSpace(scanner.Text())
				err = nil
			}
			if err != nil {
				return 3, err
			}
		}
		if !config.SafeName(version) {
			return 2, errors.New("invalid version")
		}
		path, err := config.Find(dir)
		if err == nil {
			if _, err = config.Read(path); err != nil {
				return 1, err
			}
		} else if !errors.Is(err, os.ErrNotExist) {
			return 1, err
		}
		if !m.Installed(provider, version) {
			if err = m.Install(ctx, provider, version, diagnostics); err != nil {
				return 1, err
			}
		}
		if path == "" {
			path, err = config.Init(dir)
			if err != nil {
				return 1, err
			}
		}
		if _, err = config.Init(filepath.Dir(path)); err != nil {
			return 1, err
		}
		if err = config.Bind(path, provider, version); err != nil {
			return 1, err
		}
		if err = m.ProjectLink(filepath.Dir(path), provider, version); err != nil {
			return 1, fmt.Errorf("project version saved, but SDK link failed: %w; retry svm use %s", err, provider)
		}
		fmt.Fprintf(out, "Project: %s\n%s %s\n", path, provider, version)
		return 0, nil
	case "default", "uninstall", "proxy":
		return manage(ctx, command, rest, dir, m, out, diagnostics)
	case "doctor":
		return doctor(ctx, rest, dir, m, out)
	default:
		return 2, fmt.Errorf("unknown command %q; run svm --help", command)
	}
}
func result(err error) (int, error) {
	if err != nil {
		return 1, err
	}
	return 0, nil
}
