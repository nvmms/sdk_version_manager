package process

import "os"

func Signals() []os.Signal                { return []os.Signal{os.Interrupt} }
func ExitCode(state *os.ProcessState) int { return state.ExitCode() }
