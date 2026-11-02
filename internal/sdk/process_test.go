package sdk

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/config"
)

func TestMain(m *testing.M) {
	if os.Getenv("SVM_TEST_CHILD") == "1" {
		dir, _ := os.Getwd()
		input, _ := io.ReadAll(os.Stdin)
		json.NewEncoder(os.Stdout).Encode(map[string]any{"args": os.Args[1:], "dir": dir, "input": string(input), "path": os.Getenv("PATH")})
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
	canonicalDir, err := filepath.EvalSymlinks(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got.Args, args) || got.Dir != canonicalDir || got.Input != "interactive input" || !strings.HasPrefix(got.Path, filepath.Dir(target)+string(os.PathListSeparator)) || diagnostics.String() != "child diagnostic" {
		t.Fatalf("proxy contract failed: %+v stderr=%q", got, diagnostics.String())
	}
	if os.Getenv("PATH") != original {
		t.Fatal("parent environment changed")
	}
}
