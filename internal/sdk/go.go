package sdk

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

type goFile struct {
	Filename string `json:"filename"`
	OS       string `json:"os"`
	Arch     string `json:"arch"`
	SHA256   string `json:"sha256"`
	Size     int64  `json:"size"`
	Kind     string `json:"kind"`
}
type goRelease struct {
	Version string   `json:"version"`
	Stable  bool     `json:"stable"`
	Files   []goFile `json:"files"`
}

func goArchiveName(version, goos, arch string) string {
	suffix := ".tar.gz"
	if goos == "windows" {
		suffix = ".zip"
	}
	return "go" + version + "." + goos + "-" + arch + suffix
}
func parseGoIndex(data []byte) ([]goRelease, error) {
	var releases []goRelease
	if err := json.Unmarshal(data, &releases); err != nil {
		return nil, fmt.Errorf("invalid Go index: %w", err)
	}
	if len(releases) == 0 {
		return nil, errors.New("Go index contains no releases")
	}
	for _, r := range releases {
		if !strings.HasPrefix(r.Version, "go") || !config.SafeName(r.Version) || len(r.Files) == 0 {
			return nil, errors.New("invalid Go release metadata")
		}
	}
	return releases, nil
}
func goPackage(release goRelease, goos, arch string) (goFile, bool) {
	version := strings.TrimPrefix(release.Version, "go")
	if !release.Stable || !goVersion.MatchString(version) {
		return goFile{}, false
	}
	for _, f := range release.Files {
		hash, err := hex.DecodeString(f.SHA256)
		if f.Kind == "archive" && f.OS == goos && f.Arch == arch && f.Filename == goArchiveName(version, goos, arch) && err == nil && len(hash) == 32 && f.Size > 0 {
			return f, true
		}
	}
	return goFile{}, false
}
func (m *Manager) goIndex(ctx context.Context, refresh bool, client *http.Client, url string) ([]goRelease, error) {
	path := filepath.Join(m.Root, "cache", "go", "versions.json")
	var data []byte
	var err error
	if !refresh {
		data, err = os.ReadFile(path)
		if err != nil && !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
	}
	if refresh || errors.Is(err, os.ErrNotExist) {
		data, err = fetch(ctx, client, url)
		if err != nil {
			return nil, fmt.Errorf("fetch Go versions: %w; retry svm list go --refresh or use svm list installed go offline", err)
		}
		releases, err := parseGoIndex(data)
		if err != nil {
			return nil, err
		}
		if err = ctx.Err(); err != nil {
			return nil, err
		}
		if err = config.Atomic(path, data); err != nil {
			return nil, err
		}
		return releases, nil
	}
	releases, err := parseGoIndex(data)
	if err != nil {
		return nil, fmt.Errorf("%w; run svm list go --refresh", err)
	}
	return releases, nil
}
func (m *Manager) goCatalog(ctx context.Context, refresh bool, client *http.Client, url string) ([]Version, error) {
	releases, err := m.goIndex(ctx, refresh, client, url)
	if err != nil {
		return nil, err
	}
	versions := map[string]Version{}
	for _, r := range releases {
		if _, ok := goPackage(r, runtime.GOOS, runtime.GOARCH); ok {
			version := strings.TrimPrefix(r.Version, "go")
			versions[version] = Version{Number: version, Installed: m.Installed("go", version)}
		}
	}
	installed, err := m.Versions("go")
	if err != nil {
		return nil, err
	}
	for _, v := range installed {
		versions[v] = Version{Number: v, Installed: true}
	}
	result := make([]Version, 0, len(versions))
	for _, v := range versions {
		result = append(result, v)
	}
	sort.Slice(result, func(i, j int) bool { return newer(result[i].Number, result[j].Number) })
	return result, nil
}
func (m *Manager) InstallGo(ctx context.Context, version string, out io.Writer) error {
	client, err := m.Client(30 * time.Minute)
	if err != nil {
		return err
	}
	return m.installGo(ctx, version, "https://go.dev/dl/?mode=json&include=all", "https://go.dev/dl/", client, out)
}
func (m *Manager) installGo(ctx context.Context, version, indexURL, baseURL string, client *http.Client, out io.Writer) error {
	if !goVersion.MatchString(version) {
		return errors.New("expected an exact stable Go version, for example 1.27.3 (without the go prefix)")
	}
	lock, err := filelock.Acquire(m.versionLock("go", version), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err = ctx.Err(); err != nil {
		return err
	}
	if m.Installed("go", version) {
		return nil
	}
	root, err := m.stateDirectory("installs", "go")
	if err != nil {
		return err
	}
	if _, err = os.Lstat(filepath.Join(root, version)); err == nil {
		return fmt.Errorf("incomplete Go installation; uninstall then use go %s again (unowned directories must be moved aside manually)", version)
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	releases, err := m.goIndex(ctx, false, client, indexURL)
	if err != nil {
		return err
	}
	var pkg goFile
	for _, r := range releases {
		if r.Version == "go"+version {
			pkg, _ = goPackage(r, runtime.GOOS, runtime.GOARCH)
			break
		}
	}
	if pkg.Filename == "" {
		return fmt.Errorf("Go %s has no supported %s/%s archive in the cached official index; run svm list go --refresh", version, runtime.GOOS, runtime.GOARCH)
	}
	downloadDir, err := m.stateDirectory("downloads", "go", version)
	if err != nil {
		return err
	}
	archive := filepath.Join(downloadDir, pkg.Filename+".part")
	fmt.Fprintf(out, "Installing Go %s (%s/%s)…\n", version, runtime.GOOS, runtime.GOARCH)
	if err = downloadArchive(ctx, client, baseURL+pkg.Filename, archive, pkg.SHA256, out); err != nil {
		return err
	}
	return m.publishArchive(ctx, "go", version, pkg.Filename, archive, "go", baseURL+pkg.Filename, pkg.SHA256, out)
}
