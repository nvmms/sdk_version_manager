package filelock

import (
	"path/filepath"
	"testing"
)

func TestSharedExclusiveAndRelease(t *testing.T) {
	path := filepath.Join(t.TempDir(), "resource.lock")
	a, err := Acquire(path, true)
	if err != nil {
		t.Fatal(err)
	}
	b, err := Acquire(path, true)
	if err != nil {
		t.Fatal(err)
	}
	if lock, err := Acquire(path, false); err == nil {
		lock.Close()
		t.Fatal("exclusive lock ignored readers")
	}
	a.Close()
	b.Close()
	c, err := Acquire(path, false)
	if err != nil {
		t.Fatal(err)
	}
	if lock, err := Acquire(path, true); err == nil {
		lock.Close()
		t.Fatal("reader ignored writer")
	}
	c.Close()
	d, err := Acquire(path, false)
	if err != nil {
		t.Fatal("closed lock remained active", err)
	}
	d.Close()
}
