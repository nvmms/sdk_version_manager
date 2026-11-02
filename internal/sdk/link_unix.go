//go:build !windows

package sdk

import "os"

func createDirectoryLink(path, target string) error { return os.Symlink(target, path) }
