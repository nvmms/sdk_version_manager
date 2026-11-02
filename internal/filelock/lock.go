// Package filelock provides process-scoped locks released by the OS on exit.
package filelock

import (
	"fmt"
	"os"
	"path/filepath"
)

type Lock struct{ file *os.File }

func Acquire(path string, shared bool) (*Lock, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err = acquire(f, shared); err != nil {
		f.Close()
		return nil, fmt.Errorf("resource busy (%s); retry after other SVM operations finish: %w", path, err)
	}
	return &Lock{f}, nil
}

// Lock files remain in place to avoid creating competing locks on different inodes.
func (l *Lock) Close() error { return l.file.Close() }
