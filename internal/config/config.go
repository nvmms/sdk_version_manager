// Package config owns versioned project declarations and atomic file updates.
package config

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
	"os"
	"path/filepath"
	"strings"
)

type Project struct {
	SchemaVersion int               `json:"schemaVersion"`
	SDKs          map[string]string `json:"sdks"`
}

func Read(path string) (Project, error) {
	var p Project
	b, err := os.ReadFile(path)
	if err != nil {
		return p, err
	}
	if err = json.Unmarshal(b, &p); err != nil {
		return p, fmt.Errorf("%s: invalid JSON: %w", path, err)
	}
	if p.SchemaVersion != 1 || p.SDKs == nil {
		return p, fmt.Errorf("%s: expected schemaVersion 1 and sdks object", path)
	}
	for provider, version := range p.SDKs {
		if !SafeName(provider) || !SafeName(version) {
			return p, fmt.Errorf("%s: invalid SDK binding", path)
		}
	}
	return p, nil
}

func SafeName(s string) bool {
	if s == "" || s == "." || s == ".." {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune(".-_+", c)) {
			return false
		}
	}
	return true
}

func Find(dir string) (string, error) {
	dir, err := filepath.Abs(dir)
	if err != nil {
		return "", err
	}
	for {
		path := filepath.Join(dir, ".svmrc")
		if _, err := os.Stat(path); err == nil {
			return path, nil
		} else if !errors.Is(err, os.ErrNotExist) {
			return "", err
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", os.ErrNotExist
		}
		dir = parent
	}
}

// Write uses a same-directory temporary file, so readers never see partial JSON.
func Write(path string, value any) error {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return Atomic(path, append(b, '\n'))
}

func Atomic(path string, b []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".svm-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(f.Name(), path)
}

// Update preserves unknown fields and refuses a concurrently modified declaration.
func Bind(path, provider, version string) error {
	if !SafeName(provider) || !SafeName(version) {
		return errors.New("invalid provider or version")
	}
	lock, err := filelock.Acquire(filepath.Join(filepath.Dir(path), ".svm", "project.lock"), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	original, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if _, err = Read(path); err != nil {
		return err
	}
	var object map[string]json.RawMessage
	if err = json.Unmarshal(original, &object); err != nil {
		return err
	}
	var bindings map[string]string
	if err = json.Unmarshal(object["sdks"], &bindings); err != nil {
		return err
	}
	bindings[provider] = version
	object["sdks"], err = json.Marshal(bindings)
	if err != nil {
		return err
	}
	current, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if !bytes.Equal(current, original) {
		return errors.New("project configuration changed; retry svm use")
	}
	return Write(path, object)
}

func Init(dir string) (string, error) {
	path := filepath.Join(dir, ".svmrc")
	if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
		// Publish a fully written temporary file without replacing an existing declaration.
		f, err := os.CreateTemp(dir, ".svm-init-*")
		if err != nil {
			return "", err
		}
		defer os.Remove(f.Name())
		if err = f.Chmod(0644); err != nil {
			f.Close()
			return "", err
		}
		_, writeErr := f.WriteString("{\n  \"schemaVersion\": 1,\n  \"sdks\": {}\n}\n")
		if writeErr == nil {
			writeErr = f.Sync()
		}
		closeErr := f.Close()
		if writeErr != nil {
			return "", writeErr
		}
		if closeErr != nil {
			return "", closeErr
		}
		if err = os.Link(f.Name(), path); err != nil && !errors.Is(err, os.ErrExist) {
			return "", err
		}

	} else if err != nil {
		return "", err
	}
	if _, err := Read(path); err != nil {
		return "", err
	}
	metadata := filepath.Join(dir, ".svm")
	if info, err := os.Lstat(metadata); err == nil && (!info.IsDir() || info.Mode()&os.ModeSymlink != 0) {
		return "", errors.New("refusing a linked or non-directory .svm path")
	}
	if err := os.MkdirAll(metadata, 0755); err != nil {
		return "", err
	}
	lock, err := filelock.Acquire(filepath.Join(metadata, "init.lock"), false)
	if err != nil {
		return "", err
	}
	defer lock.Close()
	ignore := filepath.Join(dir, ".gitignore")
	b, err := os.ReadFile(ignore)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	original := append([]byte(nil), b...)
	for _, line := range strings.Split(string(b), "\n") {
		if strings.TrimSpace(line) == ".svm/" {
			return path, nil
		}
	}
	if len(b) > 0 && b[len(b)-1] != '\n' {
		b = append(b, '\n')
	}
	current, readErr := os.ReadFile(ignore)
	if readErr != nil && !errors.Is(readErr, os.ErrNotExist) {
		return "", readErr
	}
	if !bytes.Equal(current, original) {
		return "", errors.New(".gitignore changed; retry svm init")
	}
	return path, Atomic(ignore, append(b, []byte(".svm/\n")...))
}
