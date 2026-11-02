package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestProjectLifecycle(t *testing.T) {
	dir := t.TempDir()
	path, err := Init(dir)
	if err != nil {
		t.Fatal(err)
	}
	original, _ := os.ReadFile(path)
	if _, err = Init(dir); err != nil {
		t.Fatal(err)
	}
	current, _ := os.ReadFile(path)
	if string(original) != string(current) {
		t.Fatal("init overwrote project")
	}
	b, _ := os.ReadFile(filepath.Join(dir, ".gitignore"))
	if strings.Count(string(b), ".svm/") != 1 {
		t.Fatal("duplicate gitignore entry")
	}
	if err = Atomic(path, []byte(`{"schemaVersion":1,"sdks":{},"custom":true}`)); err != nil {
		t.Fatal(err)
	}
	if err = Bind(path, "node", "24.0.0"); err != nil {
		t.Fatal(err)
	}
	child := filepath.Join(dir, "a", "b")
	os.MkdirAll(child, 0755)
	found, err := Find(child)
	if err != nil || found != path {
		t.Fatalf("%s %v", found, err)
	}
	p, err := Read(path)
	if err != nil || p.SDKs["node"] != "24.0.0" {
		t.Fatal(p, err)
	}
	b, _ = os.ReadFile(path)
	if !strings.Contains(string(b), `"custom": true`) {
		t.Fatal("lost unknown fields")
	}
	os.WriteFile(path, []byte("broken"), 0644)
	if _, err = Init(dir); err == nil {
		t.Fatal("accepted corrupt project")
	}
	if err = Bind(path, "node", "22.0.0"); err == nil {
		t.Fatal("rewrote corrupt project")
	}
	b, _ = os.ReadFile(path)
	if string(b) != "broken" {
		t.Fatal("corrupt project overwritten")
	}
}
func TestNearestAndSchema(t *testing.T) {
	dir := t.TempDir()
	Init(dir)
	child := filepath.Join(dir, "child")
	os.MkdirAll(child, 0755)
	Init(child)
	found, err := Find(child)
	if err != nil || found != filepath.Join(child, ".svmrc") {
		t.Fatal(found, err)
	}
	Atomic(found, []byte(`{"schemaVersion":2,"sdks":{}}`))
	if _, err = Read(found); err == nil {
		t.Fatal("accepted future schema")
	}
	for _, s := range []string{"", "..", "../x", "a/b", "a\\b"} {
		if SafeName(s) {
			t.Fatal("unsafe", s)
		}
	}
}
