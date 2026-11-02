package sdk

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

func sdkBins(provider, root, executable string) []string {
	if provider == "flutter" {
		return []string{filepath.Join(root, "bin"), filepath.Join(root, "bin", "cache", "dart-sdk", "bin")}
	}
	return []string{filepath.Dir(executable)}
}

// Windows uses bundled Dart and Flutter's own Dart entry point, avoiding cmd.exe.
// Dependency preparation is isolated to this SDK, and may need network on first run.
func (m *Manager) flutterArguments(ctx context.Context, root, version, name string, stderr io.Writer) ([]string, error) {
	if runtime.GOOS != "windows" || name != "flutter" {
		return nil, nil
	}
	lock, err := filelock.Acquire(m.versionLock("flutter-tools", version), false)
	if err != nil {
		return nil, err
	}
	defer lock.Close()
	candidates := []string{filepath.Join(root, ".dart_tool", "package_config.json"), filepath.Join(root, "packages", "flutter_tools", ".dart_tool", "package_config.json")}
	config := ""
	for _, path := range candidates {
		if info, e := os.Stat(path); e == nil && info.Mode().IsRegular() {
			config = path
			break
		}
	}
	if config == "" {
		fmt.Fprintln(stderr, "Preparing Flutter tool dependencies…")
		prepareCtx, cancel := context.WithTimeout(ctx, 10*time.Minute)
		defer cancel()
		dart := filepath.Join(root, "bin", "cache", "dart-sdk", "bin", "dart.exe")
		command := exec.CommandContext(prepareCtx, dart, "pub", "get", "--suppress-analytics")
		command.Dir = filepath.Join(root, "packages", "flutter_tools")
		command.Stdout = stderr
		command.Stderr = stderr
		command.Env = append(os.Environ(), "FLUTTER_ROOT="+root)
		if err = command.Run(); err != nil {
			return nil, fmt.Errorf("prepare Flutter tools failed: %w; check network/Git and retry svm flutter --version", err)
		}
		for _, path := range candidates {
			if _, e := os.Stat(path); e == nil {
				config = path
				break
			}
		}
		if config == "" {
			return nil, fmt.Errorf("Flutter tool package configuration is missing after dependency setup")
		}
	}
	return []string{"--packages=" + config}, nil
}
