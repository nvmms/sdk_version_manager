package terminal

import (
	"os"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

func openInput() (*os.File, error) { return os.OpenFile("CONIN$", os.O_RDONLY, 0) }
func prepareOutput(out *os.File) (func() error, error) {
	handle := windows.Handle(out.Fd())
	var mode uint32
	if err := windows.GetConsoleMode(handle, &mode); err != nil {
		return nil, err
	}
	if err := windows.SetConsoleMode(handle, mode|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING); err != nil {
		return nil, err
	}
	return func() error { return windows.SetConsoleMode(handle, mode) }, nil
}

var readConsoleInput = windows.NewLazySystemDLL("kernel32.dll").NewProc("ReadConsoleInputW")

// INPUT_RECORD with the KEY_EVENT_RECORD union at offset 4.
type consoleEvent struct {
	Type    uint16
	Padding uint16
	Down    int32
	Repeat  uint16
	Key     uint16
	Scan    uint16
	Char    uint16
	Control uint32
}

func readInput(input *os.File, b []byte) (int, error) {
	handle := windows.Handle(input.Fd())
	// Poll console events so the reader can stop before restoring terminal modes.
	ready, err := windows.WaitForSingleObject(handle, 10)
	if err != nil {
		return 0, err
	}
	if ready == uint32(windows.WAIT_TIMEOUT) {
		return 0, syscall.EAGAIN
	}
	var event consoleEvent
	var count uint32
	ok, _, callErr := readConsoleInput.Call(uintptr(handle), uintptr(unsafe.Pointer(&event)), 1, uintptr(unsafe.Pointer(&count)))
	if ok == 0 {
		return 0, callErr
	}
	if count == 0 || event.Type != 1 || event.Down == 0 {
		return 0, syscall.EAGAIN
	}
	keys := map[uint16]string{0x26: "\x1b[A", 0x28: "\x1b[B", 0x27: "\x1b[C", 0x25: "\x1b[D", 0x21: "\x1b[5~", 0x22: "\x1b[6~", 0x24: "\x1b[H", 0x23: "\x1b[F"}
	if key := keys[event.Key]; key != "" {
		return copy(b, key), nil
	}
	if event.Char == 0 {
		return 0, syscall.EAGAIN
	}
	return copy(b, string(rune(event.Char))), nil
}
