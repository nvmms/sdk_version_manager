package sdk

import (
	"encoding/binary"
	"errors"
	"os"
	"unicode/utf16"
	"unsafe"

	"golang.org/x/sys/windows"
)

// A junction needs no Developer Mode or administrator privilege.
func createDirectoryLink(path, target string) error {
	if err := os.Mkdir(path, 0755); err != nil {
		return err
	}
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		os.Remove(path)
		return err
	}
	handle, err := windows.CreateFile(p, windows.GENERIC_WRITE, 0, nil, windows.OPEN_EXISTING, windows.FILE_FLAG_OPEN_REPARSE_POINT|windows.FILE_FLAG_BACKUP_SEMANTICS, 0)
	if err != nil {
		os.Remove(path)
		return err
	}
	defer func() {
		if handle != windows.InvalidHandle {
			windows.CloseHandle(handle)
		}
	}()
	substitute := utf16.Encode([]rune(`\??\` + target))
	printName := utf16.Encode([]rune(target))
	size := 16 + (len(substitute)+1+len(printName)+1)*2
	if size > 16*1024 {
		windows.CloseHandle(handle)
		handle = windows.InvalidHandle
		os.Remove(path)
		return errors.New("junction path is too long")
	}
	buffer := make([]byte, size)
	binary.LittleEndian.PutUint32(buffer, windows.IO_REPARSE_TAG_MOUNT_POINT)
	binary.LittleEndian.PutUint16(buffer[4:], uint16(size-8))
	binary.LittleEndian.PutUint16(buffer[10:], uint16(len(substitute)*2))
	binary.LittleEndian.PutUint16(buffer[12:], uint16((len(substitute)+1)*2))
	binary.LittleEndian.PutUint16(buffer[14:], uint16(len(printName)*2))
	for i, v := range substitute {
		binary.LittleEndian.PutUint16(buffer[16+i*2:], v)
	}
	for i, v := range printName {
		binary.LittleEndian.PutUint16(buffer[16+(len(substitute)+1+i)*2:], v)
	}
	var returned uint32
	err = windows.DeviceIoControl(handle, windows.FSCTL_SET_REPARSE_POINT, (*byte)(unsafe.Pointer(&buffer[0])), uint32(len(buffer)), nil, 0, &returned, nil)
	if err != nil {
		windows.CloseHandle(handle)
		handle = windows.InvalidHandle
		os.Remove(path)
	}
	return err
}
