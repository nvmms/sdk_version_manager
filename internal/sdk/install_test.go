package sdk

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

type transport func(*http.Request) (*http.Response, error)

func (f transport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestInstallLifecycle(t *testing.T) {
	var archive bytes.Buffer
	arch := map[string]string{"amd64": "x64", "arm64": "arm64"}[runtime.GOARCH]
	platform := map[string]string{"windows": "win", "darwin": "darwin", "linux": "linux"}[runtime.GOOS]
	if arch == "" || platform == "" {
		t.Skip("unsupported platform")
	}
	prefix := "node-v24.0.0-" + platform + "-" + arch
	entry, _ := (Node{}).Entry("node")
	suffix := ".tar.gz"
	if runtime.GOOS == "windows" {
		suffix = ".zip"
		w := zip.NewWriter(&archive)
		f, _ := w.Create(prefix + "/" + entry.Executable)
		f.Write([]byte("node"))
		w.Close()
	} else {
		gz := gzip.NewWriter(&archive)
		w := tar.NewWriter(gz)
		w.WriteHeader(&tar.Header{Name: prefix + "/" + entry.Executable, Mode: 0755, Size: 4, Typeflag: tar.TypeReg})
		w.Write([]byte("node"))
		w.Close()
		gz.Close()
	}
	goodHash := fmt.Sprintf("%x", sha256.Sum256(archive.Bytes()))
	hash := strings.Repeat("0", 64)
	requests := 0
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		requests++
		body := archive.Bytes()
		if strings.HasSuffix(r.URL.Path, "SHASUMS256.txt") {
			body = []byte(hash + "  " + prefix + suffix + "\n")
		}
		return &http.Response{StatusCode: 200, Body: io.NopCloser(bytes.NewReader(body)), Header: make(http.Header)}, nil
	})}
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	install := func(ctx context.Context) error {
		return m.installNode(ctx, "24.0.0", "https://example.invalid", client, io.Discard)
	}
	if err := install(context.Background()); err == nil {
		t.Fatal("accepted bad checksum")
	}
	if m.Installed("node", "24.0.0") {
		t.Fatal("installed corrupt archive")
	}
	hash = goodHash
	if err := install(context.Background()); err != nil {
		t.Fatal(err)
	}
	n := requests
	if err := install(context.Background()); err != nil || requests != n {
		t.Fatal("repeat installation was not idempotent", err)
	}
	m.Root = t.TempDir()
	os.MkdirAll(m.Directory("node", "24.0.0"), 0755)
	if err := install(context.Background()); err == nil {
		t.Fatal("overwrote partial installation")
	}
	m.Root = t.TempDir()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := install(ctx); err == nil {
		t.Fatal("ignored cancellation")
	}
	locks, _ := filepath.Glob(filepath.Join(m.Root, "installs", "node", "*.lock"))
	if len(locks) > 0 {
		t.Fatal("lock leaked")
	}
}
func TestTarSymlinkEscape(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bad.tar.gz")
	f, _ := os.Create(path)
	gz := gzip.NewWriter(f)
	w := tar.NewWriter(gz)
	w.WriteHeader(&tar.Header{Name: "node/link", Typeflag: tar.TypeSymlink, Linkname: "../../escape"})
	w.Close()
	gz.Close()
	f.Close()
	if err := extractTar(context.Background(), path, t.TempDir()); err == nil {
		t.Fatal("accepted escaping symlink")
	}
}

func TestTarValidRelativeSymlinks(t *testing.T) {
	path := filepath.Join(t.TempDir(), "valid.tar.gz")
	f, _ := os.Create(path)
	gz := gzip.NewWriter(f)
	writer := tar.NewWriter(gz)
	for _, dir := range []string{"node/bin/", "node/lib/"} {
		writer.WriteHeader(&tar.Header{Name: dir, Typeflag: tar.TypeDir, Mode: 0755})
	}
	writer.WriteHeader(&tar.Header{Name: "node/lib/npm.js", Typeflag: tar.TypeReg, Mode: 0644, Size: 3})
	writer.Write([]byte("npm"))
	writer.WriteHeader(&tar.Header{Name: "node/bin/npm", Typeflag: tar.TypeSymlink, Linkname: "../lib/npm.js"})
	writer.Close()
	gz.Close()
	f.Close()
	if runtime.GOOS == "windows" {
		t.Skip("Unix Node.js archives use symlinks; Windows uses ZIP")
	}
	root := t.TempDir()
	if err := extractTar(context.Background(), path, root); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(filepath.Join(root, "node/bin/npm"))
	if err != nil || string(b) != "npm" {
		t.Fatal(string(b), err)
	}
}

func TestUninstallThenInstallUsesOfflineCache(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	var archive bytes.Buffer
	arch := map[string]string{"amd64": "x64", "arm64": "arm64"}[runtime.GOARCH]
	platform := map[string]string{"windows": "win", "darwin": "darwin", "linux": "linux"}[runtime.GOOS]
	prefix := "node-v24.0.0-" + platform + "-" + arch
	entry, _ := (Node{}).Entry("node")
	suffix := ".tar.gz"
	if runtime.GOOS == "windows" {
		suffix = ".zip"
		writer := zip.NewWriter(&archive)
		f, _ := writer.Create(prefix + "/" + entry.Executable)
		f.Write([]byte("node"))
		writer.Close()
	} else {
		gz := gzip.NewWriter(&archive)
		writer := tar.NewWriter(gz)
		writer.WriteHeader(&tar.Header{Name: prefix + "/" + entry.Executable, Mode: 0755, Size: 4, Typeflag: tar.TypeReg})
		writer.Write([]byte("node"))
		writer.Close()
		gz.Close()
	}
	checksum := fmt.Sprintf("%x", sha256.Sum256(archive.Bytes()))
	online := true
	requests := 0
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		requests++
		if !online {
			t.Fatal("offline reinstall tried network", r.URL)
		}
		body := archive.Bytes()
		if strings.HasSuffix(r.URL.Path, "SHASUMS256.txt") {
			body = []byte(checksum + "  " + prefix + suffix + "\n")
		}
		return &http.Response{StatusCode: 200, ContentLength: int64(len(body)), Body: io.NopCloser(bytes.NewReader(body)), Header: make(http.Header)}, nil
	})}
	if err := m.installNode(context.Background(), "24.0.0", "https://example.invalid", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	if _, err := m.Uninstall("node", "24.0.0", t.TempDir()); err != nil {
		t.Fatal(err)
	}
	cached := filepath.Join(m.Root, "downloads", "node", "24.0.0", prefix+suffix+".part")
	if _, err := os.Stat(cached); err != nil {
		t.Fatal("uninstall removed download", err)
	}
	previous := requests
	online = false
	if err := m.installNode(context.Background(), "24.0.0", "https://example.invalid", client, io.Discard); err != nil {
		t.Fatal(err)
	}
	if requests != previous || !m.Installed("node", "24.0.0") {
		t.Fatal("cache reinstall failed")
	}
}
