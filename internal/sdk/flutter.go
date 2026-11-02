package sdk

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

const flutterBase = "https://storage.googleapis.com/flutter_infra_release/releases"

type Flutter struct{}

func (Flutter) ID() string { return "flutter" }
func (Flutter) Entry(name string) (Entry, bool) {
	if name != "flutter" && name != "dart" {
		return Entry{}, false
	}
	if runtime.GOOS == "windows" {
		// Native Dart avoids shell parsing and preserves all user arguments.
		if name == "flutter" {
			return Entry{Executable: "bin/cache/dart-sdk/bin/dart.exe", Arguments: []string{"packages/flutter_tools/bin/flutter_tools.dart"}}, true
		}
		return Entry{Executable: "bin/cache/dart-sdk/bin/dart.exe"}, true
	}
	return Entry{Executable: "bin/" + name}, true
}

var flutterVersion = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?(?:\+[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$`)

func flutterOS(goos string) string {
	if goos == "darwin" {
		return "macos"
	}
	return goos
}
func flutterIndexURL() string { return flutterBase + "/releases_" + flutterOS(runtime.GOOS) + ".json" }

type flutterRelease struct {
	Version  string `json:"version"`
	Channel  string `json:"channel"`
	DartArch string `json:"dart_sdk_arch"`
	Date     string `json:"release_date"`
	Archive  string `json:"archive"`
	SHA256   string `json:"sha256"`
}
type flutterIndex struct {
	BaseURL  string           `json:"base_url"`
	Releases []flutterRelease `json:"releases"`
}

func parseFlutterIndex(data []byte) (flutterIndex, error) {
	var index flutterIndex
	if err := json.Unmarshal(data, &index); err != nil {
		return index, fmt.Errorf("invalid Flutter index: %w", err)
	}
	if index.BaseURL != flutterBase || len(index.Releases) == 0 {
		return index, errors.New("invalid Flutter index or untrusted base URL")
	}
	for _, r := range index.Releases {
		if r.Version == "" || r.Channel == "" || r.Archive == "" {
			return index, errors.New("invalid Flutter release metadata")
		}
	}
	return index, nil
}
func flutterPackage(r flutterRelease, goos, goarch string) bool {
	if r.Channel != "stable" && r.Channel != "beta" {
		return false
	}
	if !flutterVersion.MatchString(strings.TrimPrefix(r.Version, "v")) {
		return false
	}
	arch := r.DartArch
	if arch == "" {
		arch = "x64"
	}
	wanted := map[string]string{"amd64": "x64", "arm64": "arm64"}[goarch]
	if wanted == "" || arch != wanted {
		return false
	}
	u, err := url.Parse(r.Archive)
	if err != nil || u.IsAbs() || u.RawQuery != "" || u.Fragment != "" || strings.ContainsAny(r.Archive, "\\:") {
		return false
	}
	parts := strings.Split(r.Archive, "/")
	if len(parts) != 3 || parts[0] != r.Channel || parts[1] != flutterOS(goos) || !strings.HasPrefix(parts[2], "flutter_") {
		return false
	}
	suffix := ".zip"
	if goos == "linux" {
		suffix = ".tar.xz"
	}
	if !strings.HasSuffix(parts[2], suffix) {
		return false
	}
	hash, err := hex.DecodeString(r.SHA256)
	return err == nil && len(hash) == 32
}
func (m *Manager) flutterIndex(ctx context.Context, refresh bool, client *http.Client, indexURL string) (flutterIndex, error) {
	path := filepath.Join(m.Root, "cache", "flutter", "versions.json")
	var data []byte
	var err error
	if !refresh {
		data, err = os.ReadFile(path)
		if err != nil && !errors.Is(err, os.ErrNotExist) {
			return flutterIndex{}, err
		}
	}
	if refresh || errors.Is(err, os.ErrNotExist) {
		data, err = fetch(ctx, client, indexURL)
		if err != nil {
			return flutterIndex{}, fmt.Errorf("fetch Flutter versions: %w; use svm list installed flutter offline", err)
		}
		index, err := parseFlutterIndex(data)
		if err != nil {
			return index, err
		}
		if err = ctx.Err(); err != nil {
			return index, err
		}
		if err = config.Atomic(path, data); err != nil {
			return index, err
		}
		return index, nil
	}
	index, err := parseFlutterIndex(data)
	if err != nil {
		return index, fmt.Errorf("%w; run svm list flutter --refresh", err)
	}
	return index, nil
}
func (m *Manager) flutterCatalog(ctx context.Context, refresh bool, client *http.Client, indexURL string) ([]Version, error) {
	index, err := m.flutterIndex(ctx, refresh, client, indexURL)
	if err != nil {
		return nil, err
	}
	versions := map[string]Version{}
	for _, r := range index.Releases {
		if !flutterPackage(r, runtime.GOOS, runtime.GOARCH) {
			continue
		}
		v := strings.TrimPrefix(r.Version, "v")
		date := r.Date
		if len(date) > 10 {
			date = date[:10]
		}
		previous, ok := versions[v]
		if ok && previous.Channel == "stable" {
			continue
		}
		versions[v] = Version{Number: v, Date: date, Channel: r.Channel, Installed: m.Installed("flutter", v)}
	}
	installed, err := m.Versions("flutter")
	if err != nil {
		return nil, err
	}
	for _, v := range installed {
		r := versions[v]
		r.Number = v
		r.Installed = true
		versions[v] = r
	}
	result := make([]Version, 0, len(versions))
	for _, v := range versions {
		result = append(result, v)
	}
	sort.Slice(result, func(i, j int) bool { return flutterNewer(result[i].Number, result[j].Number) })
	return result, nil
}
func (m *Manager) InstallFlutter(ctx context.Context, version string, out io.Writer) error {
	client, err := m.Client(2 * time.Hour)
	if err != nil {
		return err
	}
	return m.installFlutter(ctx, version, flutterIndexURL(), client, out)
}
func (m *Manager) installFlutter(ctx context.Context, version, indexURL string, client *http.Client, out io.Writer) error {
	if !flutterVersion.MatchString(version) {
		return errors.New("expected an exact Flutter version, e.g. 3.47.9 or 3.50.0-0.1.pre")
	}
	lock, err := filelock.Acquire(m.versionLock("flutter", version), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err = ctx.Err(); err != nil {
		return err
	}
	if m.Installed("flutter", version) {
		return nil
	}
	root, err := m.stateDirectory("installs", "flutter")
	if err != nil {
		return err
	}
	if _, err = os.Lstat(filepath.Join(root, version)); err == nil {
		return errors.New("incomplete Flutter installation; uninstall it before retrying (unowned directories must be moved aside manually)")
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	index, err := m.flutterIndex(ctx, false, client, indexURL)
	if err != nil {
		return err
	}
	var pkg flutterRelease
	for _, r := range index.Releases {
		if strings.TrimPrefix(r.Version, "v") == version && flutterPackage(r, runtime.GOOS, runtime.GOARCH) {
			pkg = r
			if r.Channel == "stable" {
				break
			}
		}
	}
	if pkg.Archive == "" {
		return fmt.Errorf("Flutter %s has no %s/%s archive in the cache; run svm list flutter --refresh", version, runtime.GOOS, runtime.GOARCH)
	}
	dir, err := m.stateDirectory("downloads", "flutter", version)
	if err != nil {
		return err
	}
	name := filepath.Base(pkg.Archive)
	archive := filepath.Join(dir, name+".part")
	source := index.BaseURL + "/" + pkg.Archive
	fmt.Fprintf(out, "Installing Flutter %s (%s/%s, %s)…\n", version, runtime.GOOS, runtime.GOARCH, pkg.Channel)
	if err = downloadArchive(ctx, client, source, archive, pkg.SHA256, out); err != nil {
		return err
	}
	return m.publishArchive(ctx, "flutter", version, name, archive, "flutter", source, pkg.SHA256, out)
}

func flutterNewer(a, b string) bool {
	abase, abuild, _ := strings.Cut(a, "+")
	bbase, bbuild, _ := strings.Cut(b, "+")
	acore, apre, _ := strings.Cut(abase, "-")
	bcore, bpre, _ := strings.Cut(bbase, "-")
	if newer(acore, bcore) {
		return true
	}
	if newer(bcore, acore) {
		return false
	}
	if apre == "" && bpre != "" {
		return true
	}
	if apre != "" && bpre == "" {
		return false
	}
	if apre != bpre {
		return compareFlutterIdentifiers(apre, bpre) > 0
	}
	return compareFlutterIdentifiers(abuild, bbuild) > 0
}
func compareFlutterIdentifiers(a, b string) int {
	aa, bb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(aa) && i < len(bb); i++ {
		if aa[i] == bb[i] {
			continue
		}
		x, xerr := strconv.Atoi(aa[i])
		y, yerr := strconv.Atoi(bb[i])
		if xerr == nil && yerr == nil {
			if x > y {
				return 1
			}
			return -1
		}
		if xerr == nil {
			return -1
		}
		if yerr == nil {
			return 1
		}
		if aa[i] > bb[i] {
			return 1
		}
		return -1
	}
	if len(aa) > len(bb) {
		return 1
	}
	if len(aa) < len(bb) {
		return -1
	}
	return 0
}
