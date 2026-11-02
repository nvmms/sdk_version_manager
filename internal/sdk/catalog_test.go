package sdk

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestCatalogCacheRefreshAndInstallationState(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	key := nodeFileKey(runtime.GOOS, runtime.GOARCH)
	fixture := fmt.Sprintf(`[
 {"version":"v9.0.0","date":"2017-10-31","files":[%q],"lts":false},
 {"version":"v24.0.0","date":"2025-05-06","files":[%q],"lts":"Krypton"},
 {"version":"v25.0.0","files":["not-this-platform"],"lts":false},
 {"version":"v27.0.0-alpha.1","files":[%q],"lts":false}
 ]`, key, key, key)
	body := fixture
	requests := 0
	offline := false
	client := &http.Client{Transport: transport(func(req *http.Request) (*http.Response, error) {
		requests++
		if offline {
			return nil, errors.New("offline")
		}
		return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}, nil
	})}
	read := func(refresh bool) ([]Version, error) {
		return m.nodeCatalog(context.Background(), refresh, client, "https://example.invalid/index.json")
	}
	versions, err := read(false)
	if err != nil || len(versions) != 2 || versions[0].Number != "24.0.0" || versions[0].LTS != "Krypton" || versions[0].Installed {
		t.Fatal(versions, err)
	}
	if requests != 1 {
		t.Fatal("expected one first-load request", requests)
	}
	fakeInstall(t, m, "24.0.0")
	fakeInstall(t, m, "22.0.0")
	offline = true
	versions, err = read(false)
	if err != nil || len(versions) != 3 || !versions[0].Installed || !versions[1].Installed || requests != 1 {
		t.Fatal(versions, requests, err)
	}
	cache := filepath.Join(m.Root, "cache", "node", "versions.json")
	original, _ := os.ReadFile(cache)
	if _, err = read(true); err == nil {
		t.Fatal("refresh ignored network failure")
	}
	offline = false
	body = `[{"version":"v24.0.0","files":[],"lts":false}]`
	if _, err = read(true); err == nil {
		t.Fatal("accepted invalid refresh")
	}
	after, _ := os.ReadFile(cache)
	if string(after) != string(original) {
		t.Fatal("failed refresh overwrote valid cache")
	}
	body = strings.ReplaceAll(fixture, "24.0.0", "24.1.0")
	if _, err = read(true); err != nil {
		t.Fatal(err)
	}
	after, _ = os.ReadFile(cache)
	if string(after) != body {
		t.Fatal("refresh did not replace cache")
	}
	os.WriteFile(cache, []byte("broken"), 0600)
	n := requests
	if _, err = read(false); err == nil || requests != n {
		t.Fatal("corrupt cache was silently replaced")
	}
}
func TestNodeIndexValidation(t *testing.T) {
	for _, input := range []string{`null`, `[]`, `{}`, `[{"version":"../bad","files":["linux-x64"],"lts":false}]`, `[{"version":"v24.0.0","files":["linux-x64"],"lts":true}]`} {
		if _, err := parseNodeCatalog([]byte(input)); err == nil {
			t.Fatal("accepted", input)
		}
	}
	for _, tc := range []struct{ os, arch, key string }{{"windows", "amd64", "win-x64-zip"}, {"darwin", "arm64", "osx-arm64-tar"}, {"linux", "amd64", "linux-x64"}, {"linux", "386", ""}} {
		if got := nodeFileKey(tc.os, tc.arch); got != tc.key {
			t.Fatal(tc, got)
		}
	}
}
