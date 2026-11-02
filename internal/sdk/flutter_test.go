package sdk

import (
	"archive/tar"
	"archive/zip"
	"bytes"
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
	"github.com/ulikunitz/xz"
)

func flutterFixture(t *testing.T) ([]byte, []byte) {
	t.Helper()
	var archive bytes.Buffer
	suffix := ""
	launcher := "bin/flutter"
	dartLauncher := "bin/dart"
	if runtime.GOOS == "windows" {
		suffix = ".exe"
		launcher += ".bat"
		dartLauncher += ".bat"
	}
	files := []string{launcher, dartLauncher, "bin/cache/dart-sdk/bin/dart" + suffix, "packages/flutter_tools/bin/flutter_tools.dart"}
	ext := ".zip"
	if runtime.GOOS == "linux" {
		ext = ".tar.xz"
		encoder, err := xz.NewWriter(&archive)
		if err != nil {
			t.Fatal(err)
		}
		writer := tar.NewWriter(encoder)
		for _, name := range files {
			writer.WriteHeader(&tar.Header{Name: "flutter/" + name, Mode: 0755, Size: 7, Typeflag: tar.TypeReg})
			writer.Write([]byte("fixture"))
		}
		writer.Close()
		encoder.Close()
	} else {
		writer := zip.NewWriter(&archive)
		for _, name := range files {
			h := &zip.FileHeader{Name: "flutter/" + name, Method: zip.Deflate}
			h.SetMode(0755)
			f, _ := writer.CreateHeader(h)
			f.Write([]byte("fixture"))
		}
		writer.Close()
	}
	arch := "x64"
	if runtime.GOARCH == "arm64" {
		arch = "arm64"
	}
	release := flutterRelease{Version: "3.47.9", Channel: "stable", DartArch: arch, Date: "2026-10-29T00:00:00Z", Archive: "stable/" + flutterOS(runtime.GOOS) + "/flutter_fixture" + ext, SHA256: fmt.Sprintf("%x", sha256.Sum256(archive.Bytes()))}
	beta := release
	beta.Version = "3.50.0-0.1.pre"
	beta.Channel = "beta"
	beta.Archive = strings.Replace(release.Archive, "stable/", "beta/", 1)
	wrongArch := release
	wrongArch.Version = "3.47.8"
	wrongArch.DartArch = "unsupported"
	index, _ := json.Marshal(flutterIndex{BaseURL: flutterBase, Releases: []flutterRelease{release, beta, wrongArch}})
	return index, archive.Bytes()
}
func TestFlutterCatalogAndOfflineLifecycle(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}, Go{}, Flutter{}}}
	index, archive := flutterFixture(t)
	online := true
	requests := 0
	badRefresh := false
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		requests++
		if !online {
			t.Fatal("offline cache requested network", r.URL)
		}
		body := archive
		if r.URL.Host == "index.invalid" {
			body = index
			if badRefresh {
				body = []byte("invalid")
			}
		}
		return &http.Response{StatusCode: 200, ContentLength: int64(len(body)), Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body))}, nil
	})}
	versions, err := m.flutterCatalog(context.Background(), false, client, "https://index.invalid")
	if err != nil || len(versions) != 2 || versions[0].Number != "3.50.0-0.1.pre" || versions[1].Channel != "stable" {
		t.Fatal(versions, err)
	}
	if err = m.installFlutter(context.Background(), "3.47.9", "https://index.invalid", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	if !m.Installed("flutter", "3.47.9") {
		t.Fatal("Flutter executable not found")
	}
	if err = m.SetDefault("flutter", "3.47.9"); err != nil {
		t.Fatal(err)
	}
	if _, err = m.Uninstall("flutter", "3.47.9", t.TempDir()); err == nil {
		t.Fatal("uninstalled global default")
	}
	m.SetDefault("flutter", "")
	if _, err = m.Uninstall("flutter", "3.47.9", t.TempDir()); err != nil {
		t.Fatal(err)
	}
	online = false
	n := requests
	if err = m.installFlutter(context.Background(), "3.47.9", "https://index.invalid", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	versions, err = m.flutterCatalog(context.Background(), false, client, "https://index.invalid")
	if err != nil || !versions[1].Installed || requests != n {
		t.Fatal(versions, err)
	}
	online = true
	badRefresh = true
	if _, err = m.flutterCatalog(context.Background(), true, client, "https://index.invalid"); err == nil {
		t.Fatal("invalid refresh accepted")
	}
	online = false
	if _, err = m.flutterCatalog(context.Background(), false, client, "https://index.invalid"); err != nil {
		t.Fatal("refresh damaged cache", err)
	}
	dir := t.TempDir()
	rc, _ := config.Init(dir)
	config.Bind(rc, "flutter", "3.47.9")
	if err = m.ProjectLink(dir, "flutter", "3.47.9"); err != nil {
		t.Fatal(err)
	}
	for _, shell := range []string{"bash", "zsh", "powershell"} {
		script, err := m.Environment(dir, shell)
		if err != nil || !strings.Contains(script, "FLUTTER_ROOT") || !strings.Contains(script, "3.47.9") {
			t.Fatal(shell, script, err)
		}
	}
}
func TestFlutterReleaseValidationAndOrdering(t *testing.T) {
	valid := flutterRelease{Version: "3.47.9", Channel: "stable", DartArch: "x64", Archive: "stable/macos/flutter_macos_3.47.9-stable.zip", SHA256: strings.Repeat("0", 64)}
	if !flutterPackage(valid, "darwin", "amd64") {
		t.Fatal("rejected valid release")
	}
	if flutterPackage(valid, "darwin", "arm64") {
		t.Fatal("wrong architecture accepted")
	}
	for _, path := range []string{"../../escape.zip", "https://other.invalid/file.zip", "stable/macos/../escape.zip"} {
		r := valid
		r.Archive = path
		if flutterPackage(r, "darwin", "amd64") {
			t.Fatal("unsafe archive path", path)
		}
	}
	for _, pair := range [][2]string{{"3.50.0", "3.50.0-0.2.pre"}, {"3.50.0-0.10.pre", "3.50.0-0.2.pre"}, {"1.12.13+hotfix.10", "1.12.13+hotfix.9"}} {
		if !flutterNewer(pair[0], pair[1]) || flutterNewer(pair[1], pair[0]) {
			t.Fatal(pair)
		}
	}
}
func TestXZAndZipSafeLinks(t *testing.T) {
	// XZ extraction is exercised on every host, not only Linux builds.
	archive := filepath.Join(t.TempDir(), "sdk.tar.xz")
	f, _ := os.Create(archive)
	encoder, _ := xz.NewWriter(f)
	writer := tar.NewWriter(encoder)
	writer.WriteHeader(&tar.Header{Name: "flutter/file", Mode: 0644, Size: 2, Typeflag: tar.TypeReg})
	writer.Write([]byte("ok"))
	writer.Close()
	encoder.Close()
	f.Close()
	root := t.TempDir()
	if err := extractTar(context.Background(), archive, root); err != nil {
		t.Fatal(err)
	}
	if b, err := os.ReadFile(filepath.Join(root, "flutter/file")); err != nil || string(b) != "ok" {
		t.Fatal(string(b), err)
	}
	for _, target := range []string{"../../escape", "/tmp/escape", "C:\\escape"} {
		archive := filepath.Join(t.TempDir(), "bad.zip")
		f, _ := os.Create(archive)
		writer := zip.NewWriter(f)
		h := &zip.FileHeader{Name: "flutter/link"}
		h.SetMode(os.ModeSymlink | 0777)
		entry, _ := writer.CreateHeader(h)
		entry.Write([]byte(target))
		writer.Close()
		f.Close()
		if err := extractZip(context.Background(), archive, t.TempDir()); err == nil {
			t.Fatal("accepted escaping ZIP link", target)
		}
	}
}
