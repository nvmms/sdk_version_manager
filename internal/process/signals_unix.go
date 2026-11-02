//go:build !windows

package process

import (
	"os"
	"syscall"
)

func Signals() []os.Signal { return []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP} }
func ExitCode(state *os.ProcessState) int {
	if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
		return 128 + int(status.Signal())
	}
	return state.ExitCode()
}
