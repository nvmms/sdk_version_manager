//go:build !windows

package terminal

import (
	"os"

	"golang.org/x/sys/unix"
)

func openInput() (*os.File, error) {
	// NewFile preserves an explicitly nonblocking descriptor when Fd is queried.
	fd, err := unix.Open("/dev/tty", unix.O_RDONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(fd), "/dev/tty"), nil
}
func prepareOutput(_ *os.File) (func() error, error) { return func() error { return nil }, nil }

func readInput(input *os.File, b []byte) (int, error) {
	fd := int(input.Fd())
	if err := unix.SetNonblock(fd, true); err != nil {
		return 0, err
	}
	n, err := unix.Read(fd, b)
	if n < 0 {
		n = 0
	}
	return n, err
}
