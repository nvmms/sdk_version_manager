package sdk

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

func (m *Manager) versionLock(provider, version string) string {
	return filepath.Join(m.Root, "locks", provider+"-"+version+".lock")
}
func (m *Manager) SetDefault(provider, version string) error {
	if !m.Supports(provider) || version != "" && !ValidVersion(provider, version) {
		return errors.New("expected a supported Provider and an exact version")
	}
	lock, err := filelock.Acquire(filepath.Join(m.Root, "locks", "settings.lock"), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	defaults, err := m.Defaults()
	if err != nil {
		return err
	}
	if version == "" {
		delete(defaults, provider)
	} else {
		versionLock, err := filelock.Acquire(m.versionLock(provider, version), true)
		if err != nil {
			return err
		}
		defer versionLock.Close()
		if !m.Installed(provider, version) {
			return fmt.Errorf("not installed; run svm install %s %s", provider, version)
		}
		defaults[provider] = version
	}
	return config.Write(filepath.Join(m.Root, "settings", "default-versions.json"), defaults)
}

// ownedDirectory refuses links and anything not marked by this implementation.
// Read-only discovery still recognizes legacy installations without ownership marks.
func (m *Manager) ownedDirectory(provider, version string) (string, error) {
	if !m.Supports(provider) || !ValidVersion(provider, version) {
		return "", errors.New("invalid provider or version")
	}
	root, err := filepath.EvalSymlinks(m.Root)
	if err != nil {
		return "", err
	}
	expected := filepath.Join(root, "installs", provider, version)
	actual, err := filepath.EvalSymlinks(expected)
	if err != nil {
		return "", err
	}
	if actual != expected {
		return "", errors.New("refusing to modify a linked SDK installation")
	}
	info, err := os.Lstat(expected)
	if err != nil {
		return "", err
	}
	if !info.IsDir() {
		return "", errors.New("installation is not a directory")
	}
	home, _ := os.UserHomeDir()
	cwd, _ := os.Getwd()
	for _, protected := range []string{home, cwd, filepath.VolumeName(expected) + string(os.PathSeparator)} {
		canonical, e := filepath.EvalSymlinks(protected)
		if e != nil {
			continue
		}
		rel, e := filepath.Rel(expected, canonical)
		if e == nil && (rel == "." || rel != ".." && !strings.HasPrefix(rel, ".."+string(os.PathSeparator))) {
			return "", errors.New("refusing to modify a protected directory")
		}
	}
	marker := filepath.Join(expected, ".svm-install.json")
	markInfo, err := os.Lstat(marker)
	if errors.Is(err, os.ErrNotExist) {
		marker = m.uninstallRecord(provider, version)
		markInfo, err = os.Lstat(marker)
	}
	if err != nil {
		return "", fmt.Errorf("installation has no SVM ownership marker; it will not be removed: %w", err)
	}
	if !markInfo.Mode().IsRegular() {
		return "", errors.New("invalid installation ownership marker")
	}
	var record struct {
		SchemaVersion               int `json:"schemaVersion"`
		Provider, Version, OS, Arch string
	}
	b, err := os.ReadFile(marker)
	if err != nil {
		return "", err
	}
	if err = json.Unmarshal(b, &record); err != nil {
		return "", err
	}
	if record.SchemaVersion != 1 || record.Provider != provider || record.Version != version || record.OS != runtime.GOOS || record.Arch != runtime.GOARCH {
		return "", errors.New("installation ownership/platform marker mismatch")
	}
	return expected, nil
}

// Uninstall removes only the owned SDK directory; downloads and project declarations remain.
func (m *Manager) Uninstall(provider, version, dir string) (string, error) {
	if !m.Supports(provider) || !ValidVersion(provider, version) {
		return "", errors.New("usage: svm uninstall <node|go> <version> --yes")
	}
	lock, err := filelock.Acquire(m.versionLock(provider, version), false)
	if err != nil {
		return "", err
	}
	defer lock.Close()
	settings, err := filelock.Acquire(filepath.Join(m.Root, "locks", "settings.lock"), false)
	if err != nil {
		return "", err
	}
	defer settings.Close()
	defaults, err := m.Defaults()
	if err != nil {
		return "", err
	}
	if defaults[provider] == version {
		return "", fmt.Errorf("version is the global default; run svm default %s --clear or select another default first", provider)
	}
	path, err := config.Find(dir)
	if err == nil {
		p, e := config.Read(path)
		if e != nil {
			return "", e
		}
		if p.SDKs[provider] == version {
			return "", fmt.Errorf("version is bound by %s; select another project version first", path)
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	source, err := m.ownedDirectory(provider, version)
	if errors.Is(err, os.ErrNotExist) {
		if _, e := os.Lstat(m.Directory(provider, version)); errors.Is(e, os.ErrNotExist) {
			return "", nil
		}
	}
	if err != nil {
		return "", err
	}
	// Persist ownership outside the directory so an interrupted removal is retryable.
	record, err := os.ReadFile(filepath.Join(source, ".svm-install.json"))
	if errors.Is(err, os.ErrNotExist) {
		record, err = os.ReadFile(m.uninstallRecord(provider, version))
	}
	if err != nil {
		return "", err
	}
	if err = config.Atomic(m.uninstallRecord(provider, version), record); err != nil {
		return "", err
	}
	if err = os.RemoveAll(source); err != nil {
		return "", fmt.Errorf("uninstall partially completed; retry svm uninstall %s %s --yes: %w", provider, version, err)
	}
	if err = os.Remove(m.uninstallRecord(provider, version)); err != nil && !errors.Is(err, os.ErrNotExist) {
		return source, fmt.Errorf("SDK removed, but operation record cleanup failed: %w", err)
	}
	return source, nil
}
func (m *Manager) Health(ctx context.Context, version string) error {
	return m.HealthFor(ctx, "node", version)
}
func (m *Manager) HealthFor(ctx context.Context, provider, version string) error {
	if provider == "go" {
		return m.goHealth(ctx, version)
	}
	lock, err := filelock.Acquire(m.versionLock("node", version), true)
	if err != nil {
		return err
	}
	defer lock.Close()
	if !nodeVersion.MatchString(version) {
		return errors.New("invalid Node.js version")
	}
	if !m.Installed("node", version) {
		if _, err := os.Stat(filepath.Join(m.Directory("node", version), ".svm-install.json")); err == nil {
			return fmt.Errorf("node %s is damaged; uninstall and then run svm use node %s", version, version)
		}
		return fmt.Errorf("node %s is missing; run svm install node %s", version, version)
	}
	root := m.Directory("node", version)
	for _, name := range []string{"node", "npm", "npx"} {
		entry, _ := (Node{}).Entry(name)
		for _, relative := range append([]string{entry.Executable}, entry.Arguments...) {
			info, err := os.Stat(filepath.Join(root, relative))
			if err != nil || !info.Mode().IsRegular() {
				return fmt.Errorf("node %s has a missing %s entry; uninstall and then run svm use node %s", version, name, version)
			}
		}
	}
	return checkNodeExecutable(ctx, root, version)
}

func (m *Manager) uninstallRecord(provider, version string) string {
	return filepath.Join(m.Root, "operations", "uninstall", provider+"-"+version+".json")
}
