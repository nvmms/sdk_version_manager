package sdk

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

func goFixture(t *testing.T) ([]byte, []byte, string) {
	t.Helper()
	var archive bytes.Buffer
	entry, _ := (Go{}).Entry("go")
	format, _ := (Go{}).Entry("gofmt")
	if runtime.GOOS == "windows" {
		writer := zip.NewWriter(&archive)
		for _, exe := range []string{entry.Executable, format.Executable} {
			f, _ := writer.Create("go/" + exe)
			f.Write([]byte("fixture"))
		}
		writer.Close()
	} else {
		gz := gzip.NewWriter(&archive)
		writer := tar.NewWriter(gz)
		for _, exe := range []string{entry.Executable, format.Executable} {
			writer.WriteHeader(&tar.Header{Name: "go/" + exe, Mode: 0755, Size: 7, Typeflag: tar.TypeReg})
			writer.Write([]byte("fixture"))
		}
		writer.Close()
		gz.Close()
	}
	hash := fmt.Sprintf("%x", sha256.Sum256(archive.Bytes()))
	filename := goArchiveName("1.27.3", runtime.GOOS, runtime.GOARCH)
	releases := []goRelease{
		{Version: "go1.9", Stable: true, Files: []goFile{{Filename: goArchiveName("1.9", runtime.GOOS, runtime.GOARCH), OS: runtime.GOOS, Arch: runtime.GOARCH, SHA256: hash, Size: 1, Kind: "archive"}}},
		{Version: "go1.27rc1", Stable: false, Files: []goFile{{Filename: "go1.27rc1.src.tar.gz", Kind: "source"}}},
		{Version: "go1.27.3", Stable: true, Files: []goFile{{Filename: filename, OS: runtime.GOOS, Arch: runtime.GOARCH, SHA256: hash, Size: int64(archive.Len()), Kind: "archive"}}},
	}
	index, err := json.Marshal(releases)
	if err != nil {
		t.Fatal(err)
	}
	return index, archive.Bytes(), filename
}
func TestGoCatalogInstallCacheAndUninstall(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}, Go{}}}
	index, archive, name := goFixture(t)
	online := true
	requests := 0
	invalidRefresh := false
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		requests++
		if !online {
			t.Fatal("cached operation requested network", r.URL)
		}
		body := archive
		if r.URL.Host == "index.invalid" {
			body = index
			if invalidRefresh {
				body = []byte("invalid")
			}
		}
		return &http.Response{StatusCode: 200, ContentLength: int64(len(body)), Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body))}, nil
	})}
	catalog, err := m.goCatalog(context.Background(), false, client, "https://index.invalid")
	if err != nil || len(catalog) != 2 || catalog[0].Number != "1.27.3" || catalog[1].Number != "1.9" {
		t.Fatal(catalog, err)
	}
	if err = m.installGo(context.Background(), "1.27.3", "https://index.invalid", "https://download.invalid/", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	if !m.Installed("go", "1.27.3") {
		t.Fatal("Go executable not found")
	}
	if err = m.SetDefault("go", "1.27.3"); err != nil {
		t.Fatal(err)
	}
	if _, err = m.Uninstall("go", "1.27.3", t.TempDir()); err == nil {
		t.Fatal("uninstalled global default")
	}
	m.SetDefault("go", "")
	if _, err = m.Uninstall("go", "1.27.3", t.TempDir()); err != nil {
		t.Fatal(err)
	}
	if _, err = os.Stat(filepath.Join(m.Root, "downloads", "go", "1.27.3", name+".part")); err != nil {
		t.Fatal("lost download cache", err)
	}
	online = false
	n := requests
	if err = m.installGo(context.Background(), "1.27.3", "https://index.invalid", "https://download.invalid/", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	catalog, err = m.goCatalog(context.Background(), false, client, "https://index.invalid")
	if err != nil || !catalog[0].Installed || requests != n {
		t.Fatal(catalog, requests, err)
	}
	online = true
	invalidRefresh = true
	if _, err = m.goCatalog(context.Background(), true, client, "https://index.invalid"); err == nil {
		t.Fatal("accepted invalid refresh")
	}
	online = false
	if _, err = m.goCatalog(context.Background(), false, client, "https://index.invalid"); err != nil {
		t.Fatal("failed refresh damaged cache", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err = m.installGo(ctx, "1.26.1", "https://index.invalid", "https://download.invalid/", client, io.Discard); err == nil {
		t.Fatal("ignored cancellation")
	}
	if err = m.installGo(context.Background(), "../bad", "https://index.invalid", "https://download.invalid/", client, io.Discard); err == nil {
		t.Fatal("accepted traversal")
	}
}
func TestGoProjectEnvironmentAndDefaultsAreIndependent(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}, Go{}}}
	dir := t.TempDir()
	fakeInstall(t, m, "24.0.0")
	entry, _ := (Go{}).Entry("go")
	path := filepath.Join(m.Directory("go", "1.27.3"), entry.Executable)
	os.MkdirAll(filepath.Dir(path), 0755)
	os.WriteFile(path, []byte("fixture"), 0755)
	rc, _ := config.Init(dir)
	config.Bind(rc, "node", "24.0.0")
	config.Bind(rc, "go", "1.27.3")
	if err := m.ProjectLink(dir, "go", "1.27.3"); err != nil {
		t.Fatal(err)
	}
	if err := m.SetDefault("node", "24.0.0"); err != nil {
		t.Fatal(err)
	}
	if err := m.SetDefault("go", "1.27.3"); err != nil {
		t.Fatal(err)
	}
	defaults, _ := m.Defaults()
	if defaults["node"] != "24.0.0" || defaults["go"] != "1.27.3" {
		t.Fatal(defaults)
	}
	for _, shell := range []string{"zsh", "bash", "powershell"} {
		script, err := m.Environment(dir, shell)
		if err != nil || !strings.Contains(script, "1.27.3") || !strings.Contains(script, "24.0.0") || !strings.Contains(script, "GOROOT") || !strings.Contains(script, "GOTOOLCHAIN") {
			t.Fatal(shell, script, err)
		}
	}
	for _, version := range []string{"go1.27.3", "1.27rc1", "1.27.3/../x", ".."} {
		if ValidVersion("go", version) {
			t.Fatal("invalid version accepted", version)
		}
	}
}
