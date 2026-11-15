//go:build darwin

package cli

import (
	"os"
	"syscall"
	"unsafe"
)

// ttyState remembers the terminal settings so they can be restored.
type ttyState struct {
	termios syscall.Termios
}

func ioctl(fd uintptr, req uintptr, arg unsafe.Pointer) error {
	_, _, errno := syscall.Syscall6(syscall.SYS_IOCTL, fd, req, uintptr(arg), 0, 0, 0)
	if errno != 0 {
		return errno
	}
	return nil
}

// enterRaw puts the terminal into non-canonical, no-echo mode.
func enterRaw(f *os.File) (*ttyState, error) {
	var old syscall.Termios
	if err := ioctl(f.Fd(), syscall.TIOCGETA, unsafe.Pointer(&old)); err != nil {
		return nil, err
	}
	raw := old
	raw.Iflag &^= syscall.IXON | syscall.ICRNL | syscall.BRKINT | syscall.INPCK | syscall.ISTRIP
	raw.Lflag &^= syscall.ECHO | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	raw.Cflag |= syscall.CS8
	raw.Cc[syscall.VMIN] = 1
	raw.Cc[syscall.VTIME] = 0
	if err := ioctl(f.Fd(), syscall.TIOCSETA, unsafe.Pointer(&raw)); err != nil {
		return nil, err
	}
	return &ttyState{termios: old}, nil
}

func (s *ttyState) restore(f *os.File) {
	if s == nil {
		return
	}
	_ = ioctl(f.Fd(), syscall.TIOCSETA, unsafe.Pointer(&s.termios))
}

// readKeys forwards single key presses until stdin fails.
func readKeys(f *os.File, out chan<- byte) {
	buf := make([]byte, 1)
	for {
		n, err := f.Read(buf)
		if n > 0 {
			out <- buf[0]
		}
		if err != nil {
			return
		}
	}
}
