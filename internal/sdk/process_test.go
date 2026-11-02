package sdk

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

func TestMain(m *testing.M) {
	if os.Getenv("SVM_TEST_CHILD") == "1" {
		dir, _ := os.Getwd()
		input, _ := io.ReadAll(os.Stdin)
		json.NewEncoder(os.Stdout).Encode(map[string]any{"args": os.Args[1:], "dir": dir, "input": string(input), "path": os.Getenv("PATH"), "goroot": os.Getenv("GOROOT"), "toolchain": os.Getenv("GOTOOLCHAIN"), "flutterRoot": os.Getenv("FLUTTER_ROOT")})
		os.Stderr.WriteString("child diagnostic")
		os.Exit(17)
	}
	os.Exit(m.Run())
}
func TestProxyContract(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}}}
	dir := t.TempDir()
	source, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	entry, _ := (Node{}).Entry("node")
	target := filepath.Join(m.Directory("node", "24.0.0"), entry.Executable)
	os.MkdirAll(filepath.Dir(target), 0755)
	src, err := os.Open(source)
	if err != nil {
		t.Fatal(err)
	}
	defer src.Close()
	dst, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY, 0755)
	if err != nil {
		t.Fatal(err)
	}
	_, err = io.Copy(dst, src)
	dst.Close()
	if err != nil {
		t.Fatal(err)
	}
	path, _ := config.Init(dir)
	config.Bind(path, "node", "24.0.0")
	t.Setenv("SVM_TEST_CHILD", "1")
	original := os.Getenv("PATH")
	args := []string{"space value", "", `a"b`, "$HOME", "&|%!^()", "中文"}
	var out, diagnostics bytes.Buffer
	code, err := m.Run(context.Background(), dir, "node", args, strings.NewReader("interactive input"), &out, &diagnostics)
	if err != nil || code != 17 {
		t.Fatal(code, err, diagnostics.String())
	}
	var got struct {
		Args             []string
		Dir, Input, Path string
	}
	if err = json.Unmarshal(out.Bytes(), &got); err != nil {
		t.Fatal(err, out.String())
	}
	expectedDir, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	actualDir, err := os.Stat(got.Dir)
	if err != nil {
		t.Fatal(err)
	}
	// Windows may report the same directory through its short or long name.
	if !reflect.DeepEqual(got.Args, args) || !os.SameFile(expectedDir, actualDir) || got.Input != "interactive input" || !strings.HasPrefix(got.Path, filepath.Dir(target)+string(os.PathListSeparator)) || diagnostics.String() != "child diagnostic" {
		t.Fatalf("proxy contract failed: %+v stderr=%q", got, diagnostics.String())
	}
	if os.Getenv("PATH") != original {
		t.Fatal("parent environment changed")
	}
}

func TestGoProxyPinsGOROOTAndToolchain(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Node{}, Go{}}}
	dir := t.TempDir()
	source, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	entry, _ := (Go{}).Entry("go")
	target := filepath.Join(m.Directory("go", "1.27.3"), entry.Executable)
	os.MkdirAll(filepath.Dir(target), 0755)
	src, err := os.Open(source)
	if err != nil {
		t.Fatal(err)
	}
	defer src.Close()
	dst, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY, 0755)
	if err != nil {
		t.Fatal(err)
	}
	_, err = io.Copy(dst, src)
	dst.Close()
	if err != nil {
		t.Fatal(err)
	}
	rc, _ := config.Init(dir)
	config.Bind(rc, "go", "1.27.3")
	t.Setenv("SVM_TEST_CHILD", "1")
	t.Setenv("GOROOT", "old-root")
	t.Setenv("GOTOOLCHAIN", "auto")
	args := []string{"env", "argument with spaces", ""}
	var out, diagnostics bytes.Buffer
	code, err := m.Run(context.Background(), dir, "go", args, strings.NewReader(""), &out, &diagnostics)
	if err != nil || code != 17 {
		t.Fatal(code, err, diagnostics.String())
	}
	var result struct {
		Args              []string
		GOROOT, Toolchain string
	}
	if err = json.Unmarshal(out.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(result.Args, args) || result.GOROOT != m.Directory("go", "1.27.3") || result.Toolchain != "local" {
		t.Fatal(result)
	}
	if os.Getenv("GOROOT") != "old-root" || os.Getenv("GOTOOLCHAIN") != "auto" {
		t.Fatal("changed parent environment")
	}
}

func TestFlutterAndDartProxyArguments(t *testing.T) {
	m := &Manager{Root: t.TempDir(), Providers: []Provider{Flutter{}}}
	dir := t.TempDir()
	root := m.Directory("flutter", "3.47.9")
	source, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"flutter", "dart"} {
		entry, _ := (Flutter{}).Entry(name)
		target := filepath.Join(root, entry.Executable)
		os.MkdirAll(filepath.Dir(target), 0755)
		// Both Windows entries share Dart; copy only once.
		if _, err = os.Stat(target); err == nil {
			continue
		}
		src, e := os.Open(source)
		if e != nil {
			t.Fatal(e)
		}
		dst, e := os.OpenFile(target, os.O_CREATE|os.O_WRONLY, 0755)
		if e != nil {
			src.Close()
			t.Fatal(e)
		}
		_, e = io.Copy(dst, src)
		src.Close()
		dst.Close()
		if e != nil {
			t.Fatal(e)
		}
	}
	for _, relative := range []string{"bin/cache/flutter_tools.snapshot", "packages/flutter_tools/bin/flutter_tools.dart", ".dart_tool/package_config.json"} {
		path := filepath.Join(root, relative)
		os.MkdirAll(filepath.Dir(path), 0755)
		os.WriteFile(path, []byte("{}"), 0600)
	}
	rc, _ := config.Init(dir)
	config.Bind(rc, "flutter", "3.47.9")
	t.Setenv("SVM_TEST_CHILD", "1")
	t.Setenv("FLUTTER_ROOT", "old-root")
	args := []string{"--version", "space value", "", "%!&|^()", `a"b`}
	for _, name := range []string{"flutter", "dart"} {
		var out, diagnostics bytes.Buffer
		code, err := m.Run(context.Background(), dir, name, args, strings.NewReader(""), &out, &diagnostics)
		if err != nil || code != 17 {
			t.Fatal(name, code, err, diagnostics.String())
		}
		var result struct {
			Args        []string
			FlutterRoot string
		}
		if err = json.Unmarshal(out.Bytes(), &result); err != nil {
			t.Fatal(err)
		}
		got := result.Args
		if runtime.GOOS == "windows" && name == "flutter" {
			if len(got) != len(args)+2 || !strings.HasPrefix(got[0], "--packages=") || got[1] != filepath.Join(root, "bin/cache/flutter_tools.snapshot") {
				t.Fatal(got)
			}
			got = got[2:]
		}
		if !reflect.DeepEqual(got, args) || result.FlutterRoot != root {
			t.Fatal(result)
		}
	}
	if os.Getenv("FLUTTER_ROOT") != "old-root" {
		t.Fatal("mutated parent environment")
	}
}
