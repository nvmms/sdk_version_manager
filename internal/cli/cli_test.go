package cli

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/sdk"
)

func TestCommands(t *testing.T) {
	dir := t.TempDir()
	m := &sdk.Manager{Root: t.TempDir(), Providers: []sdk.Provider{sdk.Node{}}}
	seedCatalog(t, m)
	for _, tc := range []struct {
		args []string
		code int
	}{
		{[]string{"--help"}, 0}, {[]string{"init"}, 2}, {[]string{"init"}, 2}, {[]string{"use"}, 0},
		{[]string{"use", "node"}, 3}, {[]string{"use", "node", "../escape"}, 2},
		{[]string{"doctor"}, 0}, {[]string{"list"}, 0}, {[]string{"missing"}, 2},
		{[]string{"install", "java", "21"}, 2}, {[]string{"node", "--help"}, 3},
	} {
		var out, diagnostics bytes.Buffer
		code := Run(context.Background(), tc.args, dir, m, bytes.NewReader(nil), &out, &diagnostics)
		if code != tc.code {
			t.Fatalf("%v: code %d want %d: %s", tc.args, code, tc.code, diagnostics.String())
		}
	}
	config.Init(dir)
	config.Bind(filepath.Join(dir, ".svmrc"), "node", "24.0.0")
	child := filepath.Join(dir, "sub")
	os.Mkdir(child, 0755)
	var out, diagnostics bytes.Buffer
	if code := Run(context.Background(), []string{"doctor"}, child, m, nil, &out, &diagnostics); code != 3 {
		t.Fatal(code, diagnostics.String())
	}
}

func seedCatalog(t *testing.T, m *sdk.Manager) {
	t.Helper()
	path := filepath.Join(m.Root, "cache", "node", "versions.json")
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	data := `[{"version":"v24.0.0","date":"2025-05-06","lts":"Krypton","files":["win-x64-zip","win-arm64-zip","osx-x64-tar","osx-arm64-tar","linux-x64","linux-arm64"]},{"version":"v22.0.0","lts":false,"files":["win-x64-zip","win-arm64-zip","osx-x64-tar","osx-arm64-tar","linux-x64","linux-arm64"]}]`
	if err := os.WriteFile(path, []byte(data), 0600); err != nil {
		t.Fatal(err)
	}
}
func TestListMarksInstalledAndFilters(t *testing.T) {
	m := &sdk.Manager{Root: t.TempDir(), Providers: []sdk.Provider{sdk.Node{}}}
	seedCatalog(t, m)
	entry, _ := (sdk.Node{}).Entry("node")
	executable := filepath.Join(m.Directory("node", "24.0.0"), entry.Executable)
	if err := os.MkdirAll(filepath.Dir(executable), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(executable, []byte("fake"), 0755); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{{"list"}, {"list", "node"}, {"list", "all", "node"}, {"list", "node", "lts"}, {"list", "installed", "node"}, {"list", "node", "--installed"}} {
		var out, diagnostics bytes.Buffer
		if code := Run(context.Background(), args, t.TempDir(), m, nil, &out, &diagnostics); code != 0 {
			t.Fatal(args, code, diagnostics.String())
		}
		if !strings.Contains(out.String(), "✓") || !strings.Contains(out.String(), "24.0.0") || !strings.Contains(out.String(), "1 installed") {
			t.Fatal(args, out.String())
		}
		if len(args) > 2 && args[1] != "all" && strings.Contains(out.String(), "22.0.0") {
			t.Fatal("filter included nonmatching version", out.String())
		}
	}
	os.Remove(executable)
	var out, diagnostics bytes.Buffer
	if code := Run(context.Background(), []string{"list", "installed"}, t.TempDir(), m, nil, &out, &diagnostics); code != 0 || !strings.Contains(out.String(), "No matching versions") {
		t.Fatal(code, out.String())
	}
	for _, args := range [][]string{{"list", "go"}, {"list", "node", "--bad"}, {"list", "installed", "--refresh"}, {"list", "lts", "all"}} {
		out.Reset()
		diagnostics.Reset()
		if code := Run(context.Background(), args, t.TempDir(), m, nil, &out, &diagnostics); code != 2 {
			t.Fatal(args, code, diagnostics.String())
		}
	}
}

func TestManagementCommands(t *testing.T) {
	m := &sdk.Manager{Root: t.TempDir(), Providers: []sdk.Provider{sdk.Node{}}}
	dir := t.TempDir()
	entry, _ := (sdk.Node{}).Entry("node")
	executable := filepath.Join(m.Directory("node", "24.0.0"), entry.Executable)
	os.MkdirAll(filepath.Dir(executable), 0755)
	os.WriteFile(executable, []byte("fixture"), 0755)
	for _, tc := range []struct {
		args []string
		code int
	}{
		{[]string{"default", "node"}, 0}, {[]string{"default", "node", "24.0.0", "--no-shell"}, 0}, {[]string{"use", "node", "24.0.0", "--global", "--no-shell"}, 0},
		{[]string{"default", "node", "--clear"}, 0}, {[]string{"uninstall", "node", "24.0.0"}, 2},
		{[]string{"uninstall", "node", "24.0.0", "--yes"}, 1}, // no ownership marker
		{[]string{"proxy", "set", "http://127.0.0.1:7890"}, 0}, {[]string{"proxy", "show"}, 0}, {[]string{"proxy", "clear"}, 0},
		{[]string{"proxy", "set", "http://user:secret@localhost"}, 1}, {[]string{"env", "zsh"}, 0}, {[]string{"env", "bad"}, 1},
		{[]string{"restore", "node", "../"}, 2}, {[]string{"repair", "node", "../"}, 2},
	} {
		var out, diagnostics bytes.Buffer
		if code := Run(context.Background(), tc.args, dir, m, nil, &out, &diagnostics); code != tc.code {
			t.Fatal(tc.args, code, diagnostics.String())
		}
	}
}

func TestDefaultConfiguresShellWorkflow(t *testing.T) {
	home := t.TempDir()
	dotdir := filepath.Join(home, "dotfiles")
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	t.Setenv("SHELL", "/bin/zsh")
	t.Setenv("ZDOTDIR", dotdir)
	m := &sdk.Manager{Root: t.TempDir(), Providers: []sdk.Provider{sdk.Node{}}}
	entry, _ := (sdk.Node{}).Entry("node")
	path := filepath.Join(m.Directory("node", "24.0.0"), entry.Executable)
	os.MkdirAll(filepath.Dir(path), 0755)
	os.WriteFile(path, []byte("fixture"), 0755)
	var out, diagnostics bytes.Buffer
	for i := 0; i < 2; i++ {
		out.Reset()
		diagnostics.Reset()
		code := Run(context.Background(), []string{"default", "node", "24.0.0"}, home, m, nil, &out, &diagnostics)
		if code != 0 {
			t.Fatal(code, diagnostics.String())
		}
		if !strings.Contains(out.String(), "Shell integration ready") || !strings.Contains(out.String(), "source '") {
			t.Fatal(out.String())
		}
	}
	profile := filepath.Join(dotdir, ".zshrc")
	b, err := os.ReadFile(profile)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(b), "# >>> SVM managed environment >>>") != 1 {
		t.Fatal("hook was duplicated", string(b))
	}
	if _, err = os.Stat(filepath.Join(home, ".zshrc")); !os.IsNotExist(err) {
		t.Fatal("ignored ZDOTDIR")
	}
	// Scripted opt-out must not edit profiles.
	os.Remove(profile)
	out.Reset()
	diagnostics.Reset()
	if code := Run(context.Background(), []string{"default", "node", "24.0.0", "--no-shell"}, home, m, nil, &out, &diagnostics); code != 0 {
		t.Fatal(code, diagnostics.String())
	}
	if _, err = os.Stat(profile); !os.IsNotExist(err) {
		t.Fatal("--no-shell wrote profile")
	}
	// Partial setup is a failure with an actionable explanation, never full success.
	os.WriteFile(profile, []byte("# >>> SVM managed environment >>>\nbroken\n"), 0600)
	out.Reset()
	diagnostics.Reset()
	if code := Run(context.Background(), []string{"default", "node", "24.0.0"}, home, m, nil, &out, &diagnostics); code != 1 || !strings.Contains(diagnostics.String(), "default saved, but shell setup failed") {
		t.Fatal(code, out.String(), diagnostics.String())
	}
}

func TestPausedCommandSurface(t *testing.T) {
	m := &sdk.Manager{Root: t.TempDir(), Providers: []sdk.Provider{sdk.Node{}}}
	var out, diagnostics bytes.Buffer
	Run(context.Background(), []string{"--help"}, t.TempDir(), m, nil, &out, &diagnostics)
	for _, name := range []string{"init", "env", "shell", "ide"} {
		if strings.Contains(out.String(), "svm "+name+" ") || strings.Contains(out.String(), "svm "+name+"\n") {
			t.Fatal("paused command remains in help", name)
		}
	}
	for _, name := range []string{"init", "shell", "ide", "restore", "repair"} {
		for _, args := range [][]string{{name}, {name, "--help"}} {
			out.Reset()
			diagnostics.Reset()
			if code := Run(context.Background(), args, t.TempDir(), m, nil, &out, &diagnostics); code != 2 {
				t.Fatal(args, code)
			}
		}
	}
}
