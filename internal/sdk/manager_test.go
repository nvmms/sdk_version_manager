package sdk

import (
	"archive/zip"
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

func fakeInstall(t *testing.T, m *Manager, v string) {
	t.Helper()
	entry, _ := (Node{}).Entry("node")
	path := filepath.Join(m.Directory("node", v), entry.Executable)
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("fake"), 0755); err != nil {
		t.Fatal(err)
	}
}
func TestResolution(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	dir := t.TempDir()
	fakeInstall(t, m, "9.0.0")
	fakeInstall(t, m, "24.0.0")
	v, _, err := m.Resolve(dir, "node", true)
	if err != nil || v != "24.0.0" {
		t.Fatal(v, err)
	}
	if _, _, err = m.Resolve(dir, "node", false); err == nil {
		t.Fatal("proxy used arbitrary installed version")
	}
	config.Write(filepath.Join(m.Root, "settings", "default-versions.json"), map[string]string{"node": "22.0.0"})
	v, source, err := m.Resolve(dir, "node", false)
	if err != nil || v != "22.0.0" || source != "global default" {
		t.Fatal(v, source, err)
	}
	path, _ := config.Init(dir)
	config.Bind(path, "node", "20.0.0")
	v, source, err = m.Resolve(dir, "node", false)
	if err != nil || v != "20.0.0" || source != path {
		t.Fatal(v, source, err)
	}
	os.WriteFile(path, []byte("invalid"), 0644)
	if _, _, err = m.Resolve(dir, "node", false); err == nil {
		t.Fatal("silently ignored invalid config")
	}
}
func TestArchiveTraversalAndDuplicates(t *testing.T) {
	for _, name := range []string{"../escape", "/escape", "C:/escape", "a\\escape"} {
		if _, err := archivePath(t.TempDir(), name); err == nil {
			t.Fatal(name)
		}
	}
	for _, names := range [][]string{{"../escape"}, {"node/file", "node/file"}} {
		path := filepath.Join(t.TempDir(), "test.zip")
		file, _ := os.Create(path)
		writer := zip.NewWriter(file)
		for _, name := range names {
			w, _ := writer.Create(name)
			io.Copy(w, bytes.NewBufferString("test"))
		}
		writer.Close()
		file.Close()
		if err := extractZip(context.Background(), path, t.TempDir()); err == nil {
			t.Fatal("accepted unsafe archive")
		}
	}
}
