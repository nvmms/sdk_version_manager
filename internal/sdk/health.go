package sdk

import (
	"context"
	"fmt"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func checkNodeExecutable(ctx context.Context, root, version string) error {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	entry, _ := (Node{}).Entry("node")
	command := exec.CommandContext(ctx, filepath.Join(root, entry.Executable), "--version")
	command.Dir = root
	output, err := command.Output()
	if err != nil {
		return fmt.Errorf("node %s health check failed: %w; uninstall and then run svm use node %s", version, err, version)
	}
	if strings.TrimSpace(string(output)) != "v"+version {
		return fmt.Errorf("installed executable version mismatch (expected v%s); uninstall and then run svm use node %s", version, version)
	}
	return nil
}

func (m *Manager) goHealth(ctx context.Context, version string) error {
	if !ValidVersion("go", version) {
		return fmt.Errorf("invalid Go version")
	}
	lock, err := filelock.Acquire(m.versionLock("go", version), true)
	if err != nil {
		return err
	}
	defer lock.Close()
	root := m.Directory("go", version)
	for _, name := range []string{"go", "gofmt"} {
		entry, _ := (Go{}).Entry(name)
		info, err := os.Stat(filepath.Join(root, entry.Executable))
		if err != nil || !info.Mode().IsRegular() {
			return fmt.Errorf("Go %s has a missing %s entry; reinstall with svm use go %s", version, name, version)
		}
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	entry, _ := (Go{}).Entry("go")
	command := exec.CommandContext(ctx, filepath.Join(root, entry.Executable), "version")
	for _, v := range os.Environ() {
		key, _, _ := strings.Cut(v, "=")
		if !strings.EqualFold(key, "GOROOT") && !strings.EqualFold(key, "GOTOOLCHAIN") {
			command.Env = append(command.Env, v)
		}
	}
	command.Env = append(command.Env, "GOROOT="+root, "GOTOOLCHAIN=local")
	b, err := command.Output()
	if err != nil {
		return fmt.Errorf("Go health check failed: %w", err)
	}
	fields := strings.Fields(string(b))
	if len(fields) < 3 || fields[0] != "go" || fields[1] != "version" || fields[2] != "go"+version {
		return fmt.Errorf("Go executable version mismatch; expected go%s", version)
	}
	return nil
}
