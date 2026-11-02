package sdk

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

func ownInstall(t *testing.T, m *Manager, v string) {
	t.Helper()
	fakeInstall(t, m, v)
	err := config.Write(filepath.Join(m.Directory("node", v), ".svm-install.json"), map[string]any{"schemaVersion": 1, "provider": "node", "version": v, "os": runtime.GOOS, "arch": runtime.GOARCH})
	if err != nil {
		t.Fatal(err)
	}
}
func TestDefaultsAndUninstall(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	dir := t.TempDir()
	ownInstall(t, m, "24.0.0")
	if err := m.SetDefault("node", "24.0.0"); err != nil {
		t.Fatal(err)
	}
	if _, err := m.Uninstall("node", "24.0.0", dir); err == nil {
		t.Fatal("uninstalled global default")
	}
	if err := m.SetDefault("node", ""); err != nil {
		t.Fatal(err)
	}
	path, _ := config.Init(dir)
	config.Bind(path, "node", "24.0.0")
	if _, err := m.Uninstall("node", "24.0.0", dir); err == nil {
		t.Fatal("uninstalled bound version")
	}
	outside := t.TempDir()
	lock, err := filelock.Acquire(m.versionLock("node", "24.0.0"), true)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = m.Uninstall("node", "24.0.0", outside); err == nil {
		t.Fatal("uninstalled running SDK")
	}
	lock.Close()
	backup, err := m.Uninstall("node", "24.0.0", outside)
	if err != nil {
		t.Fatal(err)
	}
	if m.Installed("node", "24.0.0") || backup == "" {
		t.Fatal("uninstall did not detach")
	}
	if _, err = os.Stat(backup); !os.IsNotExist(err) {
		t.Fatal("installation directory was not removed", err)
	}
	if _, err = m.Uninstall("node", "24.0.0", outside); err != nil {
		t.Fatal("uninstall not idempotent", err)
	}
	fakeInstall(t, m, "22.0.0")
	if _, err = m.Uninstall("node", "22.0.0", outside); err == nil {
		t.Fatal("removed unowned installation")
	}
	if !m.Installed("node", "22.0.0") {
		t.Fatal("touched external installation")
	}
	if _, err = m.Uninstall("node", "../", outside); err == nil {
		t.Fatal("accepted traversal")
	}
}
func TestProjectLinkAndEnvironment(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	dir := t.TempDir()
	ownInstall(t, m, "24.0.0")
	ownInstall(t, m, "22.0.0")
	path, _ := config.Init(dir)
	config.Bind(path, "node", "24.0.0")
	if err := m.ProjectLink(dir, "node", "24.0.0"); err != nil {
		t.Fatal(err)
	}
	config.Bind(path, "node", "22.0.0")
	if err := m.ProjectLink(dir, "node", "22.0.0"); err != nil {
		t.Fatal(err)
	}
	if err := m.ProjectLink(dir, "node", "22.0.0"); err != nil {
		t.Fatal(err)
	}
	config.Bind(path, "node", "24.0.0")
	for _, shell := range []string{"bash", "zsh", "powershell"} {
		script, err := m.Environment(dir, shell)
		if err != nil || !strings.Contains(script, "24.0.0") || !strings.Contains(script, "SVM_ORIGINAL_PATH") {
			t.Fatal(shell, script, err)
		}
		script, err = m.Environment(t.TempDir(), shell)
		if err != nil || !strings.Contains(script, "SVM_ORIGINAL_PATH") {
			t.Fatal("restore", script, err)
		}
	}
	os.WriteFile(path, []byte("broken"), 0600)
	if _, err := m.Environment(dir, "zsh"); err == nil {
		t.Fatal("silently ignored corrupt project")
	}
	if err := m.Health(context.Background(), "24.0.0"); err == nil {
		t.Fatal("accepted incomplete SDK")
	}
}
func TestShellHookPreservesProfile(t *testing.T) {
	for _, shell := range []string{"bash", "zsh", "powershell"} {
		path := filepath.Join(t.TempDir(), "profile")
		original := "# user configuration\nexport KEEP=yes\n"
		os.WriteFile(path, []byte(original), 0600)
		for i := 0; i < 2; i++ {
			if err := writeShellHook(path, shell, "/a path/svm", true); err != nil {
				t.Fatal(err)
			}
		}
		b, _ := os.ReadFile(path)
		if strings.Count(string(b), hookStart) != 1 || !strings.Contains(string(b), original) {
			t.Fatal(string(b))
		}
		if err := writeShellHook(path, shell, "/a path/svm", false); err != nil {
			t.Fatal(err)
		}
		b, _ = os.ReadFile(path)
		if string(b) != original {
			t.Fatal("lost profile", string(b))
		}
		os.WriteFile(path, []byte(hookStart+"\nmissing end\n"), 0600)
		if err := writeShellHook(path, shell, "svm", true); err == nil {
			t.Fatal("rewrote malformed profile")
		}
	}
}
func TestNetworkSettings(t *testing.T) {
	m := &Manager{Root: t.TempDir()}
	for _, proxy := range []string{"socks5://localhost:1", "http://user:secret@localhost:1", "http://localhost:1/path"} {
		if err := m.SetProxy(proxy); err == nil {
			t.Fatal("accepted", proxy)
		}
	}
	if err := m.SetProxy("http://127.0.0.1:7890"); err != nil {
		t.Fatal(err)
	}
	got, err := m.Proxy()
	if err != nil || got != "http://127.0.0.1:7890" {
		t.Fatal(got, err)
	}
	if err = m.SetProxy(""); err != nil {
		t.Fatal(err)
	}
}

func TestUninstallRefusesEscapingDirectory(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symbolic links require privileges; junction behavior covered by project link test")
	}
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	external := t.TempDir()
	os.MkdirAll(filepath.Join(external, "24.0.0"), 0755)
	os.MkdirAll(filepath.Join(m.Root, "installs"), 0755)
	os.Symlink(external, filepath.Join(m.Root, "installs", "node"))
	if _, err := m.Uninstall("node", "24.0.0", t.TempDir()); err == nil {
		t.Fatal("accepted linked installation tree")
	}
}

func TestUnboundProjectDeactivatesShell(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	dir := t.TempDir()
	config.Init(dir)
	script, err := m.Environment(dir, "zsh")
	if err != nil || !strings.Contains(script, "unset SVM_ACTIVE_NODE") {
		t.Fatal(script, err)
	}
}

func TestUninstallInterruptedRemovalCanRetry(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	ownInstall(t, m, "24.0.0")
	source := m.Directory("node", "24.0.0")
	marker := filepath.Join(source, ".svm-install.json")
	b, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}
	if err = config.Atomic(m.uninstallRecord("node", "24.0.0"), b); err != nil {
		t.Fatal(err)
	}
	// Simulate an interrupted RemoveAll that already deleted the in-directory marker.
	os.Remove(marker)
	if _, err = m.Uninstall("node", "24.0.0", t.TempDir()); err != nil {
		t.Fatal(err)
	}
	if _, err = os.Stat(source); !os.IsNotExist(err) {
		t.Fatal("retry did not finish removal", err)
	}
}
