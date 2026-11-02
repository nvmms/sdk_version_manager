package sdk

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

func flutterLayout(root, goos string) error {
	suffix := ""
	launcher := "bin/flutter"
	if goos == "windows" {
		suffix = ".exe"
		launcher = "bin/flutter.bat"
	}
	required := []string{launcher, "bin/cache/dart-sdk/bin/dart" + suffix, "packages/flutter_tools/bin/flutter_tools.dart"}
	for _, name := range required {
		info, err := os.Stat(filepath.Join(root, name))
		if err != nil || !info.Mode().IsRegular() {
			return fmt.Errorf("Flutter SDK is missing %s", name)
		}
	}
	return nil
}
func (m *Manager) flutterHealth(ctx context.Context, version string) error {
	if !ValidVersion("flutter", version) {
		return fmt.Errorf("invalid Flutter version")
	}
	lock, err := filelock.Acquire(m.versionLock("flutter", version), true)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err = ctx.Err(); err != nil {
		return err
	}
	root := m.Directory("flutter", version)
	if err = flutterLayout(root, runtime.GOOS); err != nil {
		return fmt.Errorf("%w; reinstall with svm use flutter %s", err, version)
	}
	// Use bundled metadata rather than bootstrapping Flutter and downloading artifacts in doctor.
	path := filepath.Join(root, "bin", "cache", "flutter.version.json")
	if b, e := os.ReadFile(path); e == nil {
		var record struct {
			FrameworkVersion string `json:"frameworkVersion"`
			FlutterVersion   string `json:"flutterVersion"`
		}
		if e = json.Unmarshal(b, &record); e != nil {
			return fmt.Errorf("Flutter version metadata is invalid: %w", e)
		}
		actual := record.FrameworkVersion
		if actual == "" {
			actual = record.FlutterVersion
		}
		if actual != "" && strings.TrimPrefix(actual, "v") != version {
			return fmt.Errorf("Flutter SDK version mismatch: expected %s, found %s", version, actual)
		}
	}
	if _, err = exec.LookPath("git"); err != nil {
		if _, e := os.Stat(filepath.Join(root, "bin", "mingit", "cmd", "git.exe")); e != nil {
			return fmt.Errorf("Flutter requires Git; install Git and run svm flutter doctor")
		}
	}
	return nil
}
