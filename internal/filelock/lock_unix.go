//go:build !windows

package filelock

import (
	"golang.org/x/sys/unix"
	"os"
)

func acquire(f *os.File, shared bool) error {
	mode := unix.LOCK_EX
	if shared {
		mode = unix.LOCK_SH
	}
	return unix.Flock(int(f.Fd()), mode|unix.LOCK_NB)
}
