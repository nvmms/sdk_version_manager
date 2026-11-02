// Package sdk provides shared SDK resolution, independent of terminal presentation.
package sdk

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

var ErrNoVersion = errors.New("no SDK version selected")

type Entry struct {
	Executable string
	Arguments  []string
}
type Provider interface {
	ID() string
	Entry(name string) (Entry, bool)
}
type Node struct{}

func (Node) ID() string { return "node" }
func (Node) Entry(name string) (Entry, bool) {
	exe, prefix := "node", "lib/"
	if runtime.GOOS == "windows" {
		exe, prefix = "node.exe", ""
	} else {
		exe = "bin/node"
	}
	switch name {
	case "node":
		return Entry{Executable: exe}, true
	case "npm":
		return Entry{exe, []string{prefix + "node_modules/npm/bin/npm-cli.js"}}, true
	case "npx":
		return Entry{exe, []string{prefix + "node_modules/npm/bin/npx-cli.js"}}, true
	case "corepack":
		return Entry{exe, []string{prefix + "node_modules/corepack/dist/corepack.js"}}, true
	}
	return Entry{}, false
}

type Manager struct {
	Root      string
	Providers []Provider
}

func New() (*Manager, error) {
	root := os.Getenv("SVM_HOME")
	if root == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return nil, err
		}
		switch runtime.GOOS {
		case "windows":
			root = filepath.Join(os.Getenv("LOCALAPPDATA"), "appsdk_version_manager")
		case "darwin":
			root = filepath.Join(home, "Library", "Application Support", "appsdk_version_manager")
		default:
			root = os.Getenv("XDG_DATA_HOME")
			if root == "" {
				root = filepath.Join(home, ".local", "share")
			}
			root = filepath.Join(root, "appsdk_version_manager")
		}
	}
	root, err := filepath.Abs(root)
	if err != nil {
		return nil, err
	}
	return &Manager{Root: root, Providers: []Provider{Node{}}}, nil
}
func (m *Manager) Entry(name string) (Provider, Entry, bool) {
	for _, p := range m.Providers {
		if e, ok := p.Entry(name); ok {
			return p, e, true
		}
	}
	return nil, Entry{}, false
}
func (m *Manager) Directory(provider, version string) string {
	return filepath.Join(m.Root, "installs", provider, version)
}
func (m *Manager) Installed(provider, version string) bool {
	p, entry, ok := m.Entry(provider)
	if !ok || !config.SafeName(version) {
		return false
	}
	info, err := os.Stat(filepath.Join(m.Directory(p.ID(), version), entry.Executable))
	return err == nil && info.Mode().IsRegular()
}
func (m *Manager) Defaults() (map[string]string, error) {
	result := map[string]string{}
	b, err := os.ReadFile(filepath.Join(m.Root, "settings", "default-versions.json"))
	if errors.Is(err, os.ErrNotExist) {
		return result, nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(b, &result); err != nil {
		return nil, fmt.Errorf("invalid global defaults: %w", err)
	}
	if result == nil {
		return nil, errors.New("global defaults must be an object")
	}
	for p, v := range result {
		if !config.SafeName(p) || !config.SafeName(v) {
			return nil, errors.New("invalid global default binding")
		}
	}
	return result, nil
}
func (m *Manager) Versions(provider string) ([]string, error) {
	entries, err := os.ReadDir(filepath.Join(m.Root, "installs", provider))
	if errors.Is(err, os.ErrNotExist) {
		return []string{}, nil
	}
	if err != nil {
		return nil, err
	}
	var versions []string
	for _, e := range entries {
		if e.IsDir() && m.Installed(provider, e.Name()) {
			versions = append(versions, e.Name())
		}
	}
	sort.Slice(versions, func(i, j int) bool { return newer(versions[i], versions[j]) })
	return versions, nil
}
func newer(a, b string) bool {
	aa, bb := strings.Split(strings.TrimPrefix(a, "v"), "."), strings.Split(strings.TrimPrefix(b, "v"), ".")
	for i := 0; i < len(aa) && i < len(bb); i++ {
		x, _ := strconv.Atoi(aa[i])
		y, _ := strconv.Atoi(bb[i])
		if x != y {
			return x > y
		}
	}
	return len(aa) > len(bb)
}
func (m *Manager) Resolve(dir, provider string, latest bool) (version, source string, err error) {
	path, err := config.Find(dir)
	if err == nil {
		p, readErr := config.Read(path)
		if readErr != nil {
			return "", "", readErr
		}
		if v := p.SDKs[provider]; v != "" {
			return v, path, nil
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", "", err
	}
	defaults, err := m.Defaults()
	if err != nil {
		return "", "", err
	}
	if v := defaults[provider]; v != "" {
		return v, "global default", nil
	}
	if latest {
		versions, err := m.Versions(provider)
		if err != nil {
			return "", "", err
		}
		if len(versions) > 0 {
			return versions[0], "latest installed", nil
		}
	}
	return "", "", fmt.Errorf("%w: %s; run svm use %s <version>", ErrNoVersion, provider, provider)
}
