package sdk

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

// Version describes an installable release plus its current local installation state.
type Version struct {
	Number    string
	Date      string
	LTS       string
	Installed bool
}

type nodeRelease struct {
	Version string          `json:"version"`
	Date    string          `json:"date"`
	Files   []string        `json:"files"`
	LTS     json.RawMessage `json:"lts"`
}

// Catalog reads the cached official index. Only a missing cache or explicit refresh uses the network.
func (m *Manager) Catalog(ctx context.Context, provider string, refresh bool) ([]Version, error) {
	if provider != "node" {
		return nil, fmt.Errorf("Provider %q is not migrated yet; currently supported: node", provider)
	}
	client, err := m.Client(30 * time.Second)
	if err != nil {
		return nil, err
	}
	return m.nodeCatalog(ctx, refresh, client, "https://nodejs.org/dist/index.json")
}

func nodeFileKey(goos, goarch string) string {
	arch := map[string]string{"amd64": "x64", "arm64": "arm64"}[goarch]
	if arch == "" {
		return ""
	}
	switch goos {
	case "windows":
		return "win-" + arch + "-zip"
	case "darwin":
		return "osx-" + arch + "-tar"
	case "linux":
		return "linux-" + arch
	}
	return ""
}

func parseNodeCatalog(data []byte) ([]nodeRelease, error) {
	var releases []nodeRelease
	if err := json.Unmarshal(data, &releases); err != nil {
		return nil, fmt.Errorf("invalid Node.js index: %w", err)
	}
	if len(releases) == 0 {
		return nil, errors.New("Node.js index contains no releases")
	}
	for _, r := range releases {
		// Prereleases are not installable by the current Provider; keep them in the raw cache.
		version := strings.TrimPrefix(r.Version, "v")
		if !config.SafeName(version) || len(r.Files) == 0 {
			return nil, errors.New("Node.js index contains an invalid release")
		}
		if string(r.LTS) != "false" {
			var lts string
			if err := json.Unmarshal(r.LTS, &lts); err != nil || lts == "" {
				return nil, errors.New("Node.js index contains invalid LTS metadata")
			}
		}
	}
	return releases, nil
}

func (m *Manager) nodeCatalog(ctx context.Context, refresh bool, client *http.Client, url string) ([]Version, error) {
	fileKey := nodeFileKey(runtime.GOOS, runtime.GOARCH)
	if fileKey == "" {
		return nil, errors.New("unsupported Node.js platform or architecture")
	}
	cache := filepath.Join(m.Root, "cache", "node", "versions.json")
	var data []byte
	var releases []nodeRelease
	var err error
	if !refresh {
		data, err = os.ReadFile(cache)
		if err != nil && !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
	}
	if refresh || errors.Is(err, os.ErrNotExist) {
		data, err = fetch(ctx, client, url)
		if err != nil {
			return nil, fmt.Errorf("fetch Node.js versions: %w; retry svm list node --refresh or use svm list installed node offline", err)
		}
		releases, err = parseNodeCatalog(data)
		if err != nil {
			return nil, err
		}
		if err = ctx.Err(); err != nil {
			return nil, err
		}
		if err = config.Atomic(cache, data); err != nil {
			return nil, fmt.Errorf("save Node.js index: %w", err)
		}
	} else {
		releases, err = parseNodeCatalog(data)
		if err != nil {
			return nil, fmt.Errorf("%w; run svm list node --refresh to repair the cache", err)
		}
	}
	versions := make(map[string]Version)
	for _, r := range releases {
		number := strings.TrimPrefix(r.Version, "v")
		if !nodeVersion.MatchString(number) {
			continue
		}
		for _, file := range r.Files {
			if file != fileKey {
				continue
			}
			var lts string
			if string(r.LTS) != "false" {
				_ = json.Unmarshal(r.LTS, &lts)
			}
			versions[number] = Version{Number: number, Date: r.Date, LTS: lts, Installed: m.Installed("node", number)}
			break
		}
	}
	// Preserve visibility of local installations missing from the current index/platform.
	installed, err := m.Versions("node")
	if err != nil {
		return nil, err
	}
	for _, number := range installed {
		v := versions[number]
		v.Number = number
		v.Installed = true
		versions[number] = v
	}
	result := make([]Version, 0, len(versions))
	for _, v := range versions {
		result = append(result, v)
	}
	sort.Slice(result, func(i, j int) bool { return newer(result[i].Number, result[j].Number) })
	return result, nil
}
