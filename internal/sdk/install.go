package sdk

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

var nodeVersion = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)

// InstallNode verifies the official SHA-256 before extracting into a private staging directory.
// Existing installations are never removed or replaced.
func (m *Manager) InstallNode(ctx context.Context, version string, progress io.Writer) error {
	client, err := m.Client(30 * time.Minute)
	if err != nil {
		return err
	}
	return m.installNode(ctx, version, "https://nodejs.org/dist", client, progress)
}
func (m *Manager) installNode(ctx context.Context, version, base string, client *http.Client, progress io.Writer) error {
	if !nodeVersion.MatchString(version) {
		return errors.New("Node.js version must be an exact version, for example 24.0.0")
	}
	lock, err := filelock.Acquire(m.versionLock("node", version), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	return m.installNodeLocked(ctx, version, base, client, progress)
}
func (m *Manager) installNodeLocked(ctx context.Context, version, base string, client *http.Client, progress io.Writer) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if m.Installed("node", version) {
		return nil
	}
	arch := map[string]string{"amd64": "x64", "arm64": "arm64"}[runtime.GOARCH]
	platform := map[string]string{"windows": "win", "darwin": "darwin", "linux": "linux"}[runtime.GOOS]
	if arch == "" || platform == "" {
		return errors.New("unsupported Node.js platform or architecture")
	}
	root, err := m.stateDirectory("installs", "node")
	if err != nil {
		return err
	}
	destination := filepath.Join(root, version)
	if _, err := os.Lstat(destination); err == nil {
		if _, e := os.Stat(filepath.Join(destination, ".svm-install.json")); e == nil {
			return fmt.Errorf("incomplete owned installation; uninstall then use node %s again", version)
		}
		return fmt.Errorf("unowned incomplete installation at %s; move it aside before retrying", destination)
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	suffix := ".tar.gz"
	if runtime.GOOS == "windows" {
		suffix = ".zip"
	}
	name := "node-v" + version + "-" + platform + "-" + arch + suffix
	url := base + "/v" + version + "/"
	downloadDir, err := m.stateDirectory("downloads", "node", version)
	if err != nil {
		return err
	}
	partial := filepath.Join(downloadDir, name+".part")
	checksumPath := filepath.Join(downloadDir, "SHASUMS256.txt")
	checksums, _ := os.ReadFile(checksumPath)
	expected := archiveChecksum(checksums, name)
	if expected == "" || !verifiedArchive(partial, expected) {
		checksums, err = fetch(ctx, client, url+"SHASUMS256.txt")
		if err != nil {
			return err
		}
		expected = archiveChecksum(checksums, name)
		if expected == "" {
			return errors.New("official SHA-256 checksum missing or invalid")
		}
	}
	fmt.Fprintf(progress, "Installing Node.js %s (%s/%s)…\n", version, runtime.GOOS, runtime.GOARCH)
	if err = downloadArchive(ctx, client, url+name, partial, expected, progress); err != nil {
		return err
	}
	if err = config.Atomic(checksumPath, checksums); err != nil {
		return err
	}
	return m.publishArchive(ctx, "node", version, name, partial, strings.TrimSuffix(name, suffix), url+name, expected, progress)
}

// publishArchive is the common validated extraction/publish step for archive Providers.
func (m *Manager) publishArchive(ctx context.Context, provider, version, name, archive, archiveRoot, source, checksum string, out io.Writer) error {
	root, err := m.stateDirectory("installs", provider)
	if err != nil {
		return err
	}
	destination := filepath.Join(root, version)
	fmt.Fprintln(out, "Checksum verified; extracting…")
	staging, err := os.MkdirTemp(root, ".extracting-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(staging)
	if strings.HasSuffix(name, ".zip") {
		err = extractZip(ctx, archive, staging)
	} else {
		err = extractTar(ctx, archive, staging)
	}
	if err != nil {
		return err
	}
	extracted := filepath.Join(staging, archiveRoot)
	_, entry, ok := m.Entry(provider)
	if !ok {
		return errors.New("Provider entry point missing")
	}
	info, err := os.Stat(filepath.Join(extracted, entry.Executable))
	if err != nil || !info.Mode().IsRegular() {
		return fmt.Errorf("archive does not contain the expected %s executable", provider)
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	record := map[string]any{"schemaVersion": 1, "provider": provider, "version": version, "os": runtime.GOOS, "arch": runtime.GOARCH, "source": source, "sha256": checksum}
	if err = config.Write(filepath.Join(extracted, ".svm-install.json"), record); err != nil {
		return err
	}
	if err = os.Rename(extracted, destination); err != nil {
		return err
	}
	fmt.Fprintf(out, "Installed %s %s: %s\n", provider, version, destination)
	return nil
}

func fetch(ctx context.Context, client *http.Client, url string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	response, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("index: HTTP %d", response.StatusCode)
	}
	const limit = 16 << 20
	b, err := io.ReadAll(io.LimitReader(response.Body, limit+1))
	if len(b) > limit {
		return nil, errors.New("index exceeds size limit")
	}
	return b, err
}
func archivePath(root, name string) (string, error) {
	if strings.ContainsAny(name, "\\:") || strings.HasPrefix(name, "/") {
		return "", fmt.Errorf("unsafe archive path %q", name)
	}
	clean := filepath.Clean(filepath.FromSlash(name))
	if clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) || filepath.IsAbs(clean) {
		return "", fmt.Errorf("archive path escapes destination: %q", name)
	}
	return filepath.Join(root, clean), nil
}
func writeEntry(path string, mode os.FileMode, r io.Reader) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode.Perm()&0755)
	if err != nil {
		return err
	}
	_, err = io.Copy(file, r)
	closeErr := file.Close()
	if err != nil {
		return err
	}
	return closeErr
}
func extractZip(ctx context.Context, archive, root string) error {
	var canonicalErr error
	root, canonicalErr = filepath.EvalSymlinks(root)
	if canonicalErr != nil {
		return canonicalErr
	}
	reader, err := zip.OpenReader(archive)
	if err != nil {
		return err
	}
	defer reader.Close()
	for _, f := range reader.File {
		if err := ctx.Err(); err != nil {
			return err
		}
		path, err := archivePath(root, f.Name)
		if err != nil {
			return err
		}
		if f.Mode()&os.ModeSymlink != 0 {
			return errors.New("ZIP symbolic links are not supported")
		}
		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(path, 0755); err != nil {
				return err
			}
			continue
		}
		r, err := f.Open()
		if err != nil {
			return err
		}
		err = writeEntry(path, f.Mode(), r)
		r.Close()
		if err != nil {
			return err
		}
	}
	return nil
}
func extractTar(ctx context.Context, archive, root string) error {
	var canonicalErr error
	root, canonicalErr = filepath.EvalSymlinks(root)
	if canonicalErr != nil {
		return canonicalErr
	}
	file, err := os.Open(archive)
	if err != nil {
		return err
	}
	defer file.Close()
	gz, err := gzip.NewReader(file)
	if err != nil {
		return err
	}
	defer gz.Close()
	reader := tar.NewReader(gz)
	type link struct{ path, target string }
	var links []link
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		h, err := reader.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		path, err := archivePath(root, h.Name)
		if err != nil {
			return err
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err = os.MkdirAll(path, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err = writeEntry(path, os.FileMode(h.Mode), reader); err != nil {
				return err
			}
		case tar.TypeSymlink:
			// Create links last: archive members cannot write through a link to escape staging.
			if _, err = archivePath(root, filepath.ToSlash(filepath.Join(filepath.Dir(h.Name), h.Linkname))); err != nil || filepath.IsAbs(h.Linkname) || strings.ContainsAny(h.Linkname, "\\:") {
				return errors.New("unsafe archive symbolic link")
			}
			links = append(links, link{path, h.Linkname})
		default:
			return fmt.Errorf("unsupported archive entry %q", h.Name)
		}
	}
	for _, l := range links {
		// Parents must already be real directories; never follow an earlier archive link.
		parent := filepath.Dir(l.path)
		for parent != root {
			info, statErr := os.Lstat(parent)
			if statErr != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
				return errors.New("unsafe or missing symbolic link parent")
			}
			next := filepath.Dir(parent)
			if next == parent {
				return errors.New("symbolic link parent escapes staging")
			}
			parent = next
		}
		if err = os.Symlink(l.target, l.path); err != nil {
			return err
		}
	}
	for _, l := range links {
		resolved, err := filepath.EvalSymlinks(l.path)
		if err != nil {
			return fmt.Errorf("invalid archive symbolic link: %w", err)
		}
		relative, err := filepath.Rel(root, resolved)
		if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(os.PathSeparator)) {
			return errors.New("archive symbolic link escapes staging")
		}
	}

	return nil
}

func archiveChecksum(document []byte, name string) string {
	for _, line := range strings.Split(string(document), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 2 && fields[1] == name {
			decoded, err := hex.DecodeString(fields[0])
			if err == nil && len(decoded) == sha256.Size {
				return fields[0]
			}
		}
	}
	return ""
}
func verifiedArchive(path, expected string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	hash := sha256.New()
	if _, err = io.Copy(hash, file); err != nil {
		return false
	}
	return strings.EqualFold(hex.EncodeToString(hash.Sum(nil)), expected)
}
