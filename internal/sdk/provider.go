package sdk

import (
	"context"
	"fmt"
	"io"
	"regexp"
	"runtime"
)

type Go struct{}

func (Go) ID() string { return "go" }
func (Go) Entry(name string) (Entry, bool) {
	if name != "go" && name != "gofmt" {
		return Entry{}, false
	}
	suffix := ""
	if runtime.GOOS == "windows" {
		suffix = ".exe"
	}
	return Entry{Executable: "bin/" + name + suffix}, true
}

var goVersion = regexp.MustCompile(`^1\.[0-9]+(?:\.[0-9]+)?$`)

func ValidVersion(provider, version string) bool {
	switch provider {
	case "node":
		return nodeVersion.MatchString(version)
	case "go":
		return goVersion.MatchString(version)
	case "flutter":
		return flutterVersion.MatchString(version)
	}
	return false
}
func (m *Manager) Supports(provider string) bool {
	for _, p := range m.Providers {
		if p.ID() == provider {
			return true
		}
	}
	return false
}
func (m *Manager) Install(ctx context.Context, provider, version string, out io.Writer) error {
	if !m.Supports(provider) {
		return fmt.Errorf("unsupported Provider %q", provider)
	}
	switch provider {
	case "node":
		return m.InstallNode(ctx, version, out)
	case "go":
		return m.InstallGo(ctx, version, out)
	case "flutter":
		return m.InstallFlutter(ctx, version, out)
	}
	return fmt.Errorf("installation unsupported for %s", provider)
}
