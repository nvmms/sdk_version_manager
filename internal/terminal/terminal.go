// Package terminal owns raw-mode setup and restoration for interactive CLI views.
package terminal

import (
	"errors"
	"io"
	"os"

	"golang.org/x/term"
)

type Session struct {
	Input         *os.File
	Output        *os.File
	state         *term.State
	restoreOutput func() error
}

func Supported(in io.Reader, out io.Writer) bool {
	input, ok := in.(*os.File)
	if !ok {
		return false
	}
	output, ok := out.(*os.File)
	if !ok {
		return false
	}
	return os.Getenv("TERM") != "dumb" && term.IsTerminal(int(input.Fd())) && term.IsTerminal(int(output.Fd()))
}
func Open(out *os.File) (*Session, error) {
	input, err := openInput()
	if err != nil {
		return nil, err
	}
	state, err := term.MakeRaw(int(input.Fd()))
	if err != nil {
		input.Close()
		return nil, err
	}
	restore, err := prepareOutput(out)
	if err != nil {
		term.Restore(int(input.Fd()), state)
		input.Close()
		return nil, err
	}
	s := &Session{Input: input, Output: out, state: state, restoreOutput: restore}
	if _, err = io.WriteString(out, "\x1b[?1049h\x1b[?25l"); err != nil {
		s.Close()
		return nil, err
	}
	return s, nil
}
func (s *Session) Size() (int, int) {
	w, h, err := term.GetSize(int(s.Output.Fd()))
	if err != nil || w < 1 || h < 1 {
		return 80, 24
	}
	return w, h
}
func (s *Session) Close() error {
	_, outputErr := io.WriteString(s.Output, "\x1b[0m\x1b[?25h\x1b[?1049l")
	rawErr := term.Restore(int(s.Input.Fd()), s.state)
	closeErr := s.Input.Close()
	return errors.Join(outputErr, rawErr, closeErr, s.restoreOutput())
}

func (s *Session) Read(b []byte) (int, error) { return readInput(s.Input, b) }
