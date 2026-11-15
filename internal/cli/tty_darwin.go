//go:build darwin

package cli

import (
	"io"
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

// Logical keys for the arrow keys; values above ASCII so they cannot collide.
const (
	keyLeft byte = 0x80 + iota
	keyRight
	keyUp
	keyDown
)

// readKeys forwards single key presses until stdin fails. ANSI arrow
// sequences are decoded into the key* constants.
func readKeys(f *os.File, out chan<- byte) {
	buf := make([]byte, 1)
	for {
		n, err := f.Read(buf)
		if n > 0 {
			if buf[0] == 0x1b {
				seq := make([]byte, 2)
				if _, err := io.ReadFull(f, seq); err == nil && seq[0] == '[' {
					switch seq[1] {
					case 'A':
						out <- keyUp
					case 'B':
						out <- keyDown
					case 'C':
						out <- keyRight
					case 'D':
						out <- keyLeft
					}
					continue
				}
				out <- 0x1b
				continue
			}
			out <- buf[0]
		}
		if err != nil {
			return
		}
	}
}
