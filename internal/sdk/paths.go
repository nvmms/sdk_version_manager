package sdk

import (
	"errors"
	"os"
	"path/filepath"
)

// stateDirectory creates only real subdirectories below the canonical state root.
func (m *Manager) stateDirectory(parts ...string) (string, error) {
	if err := os.MkdirAll(m.Root, 0755); err != nil {
		return "", err
	}
	root, err := filepath.EvalSymlinks(m.Root)
	if err != nil {
		return "", err
	}
	current := root
	for _, part := range parts {
		if part == "" || part == "." || part == ".." || filepath.Base(part) != part {
			return "", errors.New("invalid state path component")
		}
		current = filepath.Join(current, part)
		info, err := os.Lstat(current)
		if errors.Is(err, os.ErrNotExist) {
			if err = os.Mkdir(current, 0755); err != nil {
				return "", err
			}
		} else if err != nil {
			return "", err
		} else if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "", errors.New("state path contains a link or non-directory: " + current)
		}
	}
	return current, nil
}
