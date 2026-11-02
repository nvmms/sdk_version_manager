package sdk

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

func (m *Manager) ProjectLink(project, provider, version string) error {
	if !m.Supports(provider) || !ValidVersion(provider, version) {
		return errors.New("invalid SDK binding")
	}
	if !m.Installed(provider, version) {
		return fmt.Errorf("SDK missing; run svm install %s %s", provider, version)
	}
	lock, err := filelock.Acquire(m.versionLock(provider, version), true)
	if err != nil {
		return err
	}
	defer lock.Close()
	for _, dir := range []string{filepath.Join(project, ".svm"), filepath.Join(project, ".svm", "sdks")} {
		info, err := os.Lstat(dir)
		if errors.Is(err, os.ErrNotExist) {
			if err = os.Mkdir(dir, 0755); err != nil {
				return err
			}
		} else if err != nil {
			return err
		} else if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("refusing linked or non-directory project metadata: %s", dir)
		}
	}
	projectLock, err := filelock.Acquire(filepath.Join(project, ".svm", "project.lock"), false)
	if err != nil {
		return err
	}
	defer projectLock.Close()
	declaration, err := config.Read(filepath.Join(project, ".svmrc"))
	if err != nil {
		return err
	}
	if selected := declaration.SDKs[provider]; selected != "" && selected != version {
		return errors.New("project version changed; retry svm use")
	}
	path := filepath.Join(project, ".svm", "sdks", provider)
	target, err := filepath.EvalSymlinks(m.Directory(provider, version))
	if err != nil {
		return err
	}
	marker := filepath.Join(project, ".svm", provider+"-link.json")
	if current, err := filepath.EvalSymlinks(path); err == nil && current == target {
		return config.Write(marker, map[string]string{"target": target})
	}
	if _, err := os.Lstat(path); err == nil {
		// Readlink recognizes both Unix symlinks and Windows junctions.
		// Junctions need not carry ModeSymlink in FileInfo.
		actual, linkErr := os.Readlink(path)
		if linkErr != nil {
			return errors.New("project SDK path is not a link; it was not overwritten")
		}
		// A different link may belong to the user. Only replace a recorded SVM link.
		var owned struct {
			Target string `json:"target"`
		}
		b, e := os.ReadFile(marker)
		if e != nil {
			return errors.New("project SDK link is not owned by SVM")
		}
		if e = json.Unmarshal(b, &owned); e != nil {
			return e
		}
		if !filepath.IsAbs(actual) {
			actual = filepath.Join(filepath.Dir(path), actual)
		}
		if filepath.Clean(actual) != filepath.Clean(owned.Target) {
			return errors.New("project SDK link changed outside SVM; it was not overwritten")
		}
		if err = os.Remove(path); err != nil {
			return err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if err = createDirectoryLink(path, target); err != nil {
		return err
	}
	return config.Write(marker, map[string]string{"target": target})
}
