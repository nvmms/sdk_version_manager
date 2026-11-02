package sdk

import (
	"context"
	"errors"
	"fmt"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
	"github.com/nvmms/sdk_version_manager/internal/process"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
)

func (m *Manager) Run(ctx context.Context, dir, name string, args []string, stdin io.Reader, stdout, stderr io.Writer) (int, error) {
	provider, entry, ok := m.Entry(name)
	if !ok {
		return 2, fmt.Errorf("unknown SDK entry point %q", name)
	}
	version, _, err := m.Resolve(dir, provider.ID(), false)
	if err != nil {
		return 3, err
	}
	lock, err := filelock.Acquire(m.versionLock(provider.ID(), version), true)
	if err != nil {
		return 1, err
	}
	defer lock.Close()
	if !m.Installed(provider.ID(), version) {
		return 3, fmt.Errorf("%s %s is not installed; run svm install %s %s", provider.ID(), version, provider.ID(), version)
	}
	root := m.Directory(provider.ID(), version)
	executable := filepath.Join(root, entry.Executable)
	if provider.ID() == "flutter" && runtime.GOOS == "windows" && name == "flutter" {
		if info, e := os.Stat(filepath.Join(root, "bin", "cache", "flutter_tools.snapshot")); e == nil && info.Mode().IsRegular() {
			entry.Arguments = []string{"bin/cache/flutter_tools.snapshot"}
		}
	}
	leading := make([]string, len(entry.Arguments))
	for i, arg := range entry.Arguments {
		leading[i] = filepath.Join(root, filepath.FromSlash(arg))
		if _, err := os.Stat(leading[i]); err != nil {
			if name == "corepack" {
				return 3, errors.New("Corepack is not bundled in this SDK; install it with svm npm install --global corepack")
			}
			return 3, fmt.Errorf("SDK entry point missing: %s; uninstall and then run svm use node %s", leading[i], version)
		}
	}
	if provider.ID() == "flutter" {
		extra, err := m.flutterArguments(ctx, root, version, name, stderr)
		if err != nil {
			return 1, err
		}
		leading = append(extra, leading...)
	}
	command := exec.Command(executable, append(leading, args...)...)
	command.Dir = dir
	command.Stdin = stdin
	command.Stdout = stdout
	command.Stderr = stderr
	// Replace PATH rather than appending a second key (Windows keys are case insensitive).
	for _, v := range os.Environ() {
		key, _, _ := strings.Cut(v, "=")
		if !strings.EqualFold(key, "PATH") && !(provider.ID() == "go" && (strings.EqualFold(key, "GOROOT") || strings.EqualFold(key, "GOTOOLCHAIN"))) && !(provider.ID() == "flutter" && strings.EqualFold(key, "FLUTTER_ROOT")) {
			command.Env = append(command.Env, v)
		}
	}
	if provider.ID() == "go" {
		command.Env = append(command.Env, "GOROOT="+root, "GOTOOLCHAIN=local")
	}
	bins := sdkBins(provider.ID(), root, executable)
	if provider.ID() == "flutter" {
		command.Env = append(command.Env, "FLUTTER_ROOT="+root)
		if runtime.GOOS == "windows" {
			bins = append(bins, filepath.Join(root, "bin", "mingit", "cmd"))
		}
	}
	command.Env = append(command.Env, "PATH="+strings.Join(bins, string(os.PathListSeparator))+string(os.PathListSeparator)+os.Getenv("PATH"))
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, process.Signals()...)
	defer signal.Stop(signals)
	if err := command.Start(); err != nil {
		return 1, err
	}
	done := make(chan struct{})
	defer close(done)
	go func() {
		for {
			select {
			case sig := <-signals:
				_ = command.Process.Signal(sig)
			case <-ctx.Done():
				_ = command.Process.Kill()
				return
			case <-done:
				return
			}
		}
	}()
	err = command.Wait()
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		code := process.ExitCode(exit.ProcessState)
		if code < 0 {
			code = 1
		}
		return code, nil
	}
	if err != nil {
		return 1, err
	}
	return 0, nil
}
